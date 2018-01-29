#include <WinError.h>
#include "RingBuffer.h"
#include "KLogger.h"

#define FLUSH_THRESHOLD 50u // in percents
#define DEFAULT_RING_BUF_SIZE (100ull * 1024ull * 1024ull)
#define FLUSH_BUF_SIZE DEFAULT_RING_BUF_SIZE
#define REGISTRY_BUF_SIZE_KEY L"BUF_SIZE"
#define FLUSH_TIMEOUT 10000000ll
#define START_TIMEOUT 50000000ll

typedef struct KLogger
{
	PRINGBUFFER pRingBuf;

	HANDLE FileHandle;
	PCHAR pFlushingBuf;

	HANDLE FlushingThreadHandle;
	PKTHREAD pFlushingThread;

	KEVENT FlushEvent;
	KEVENT StartEvent;
	KEVENT StopEvent;

	LONG volatile IsFlushDispatched;
	PKDPC pFlushDpc;

} KLOGGER;

PKLOGGER gKLogger;

VOID SetWriteEvent(
	IN PKDPC pthisDpcObject,
	IN PVOID DeferredContext,
	IN PVOID SystemArgument1,
	IN PVOID SystemArgument2
);

static INT 
WriteToFile(
	HANDLE FileHandle,
	PVOID Buf, 
	SIZE_T Length
) {
	IO_STATUS_BLOCK IoStatusBlock;
	NTSTATUS Status = ZwWriteFile(
		FileHandle,
		NULL,
		NULL,
		NULL,
		&IoStatusBlock,
		Buf,
		(ULONG)Length,
		NULL,
		NULL
	);

	return Status;
}

VOID 
FlushingThreadFunc(
	IN PVOID _Unused
) {
	UNREFERENCED_PARAMETER(_Unused);
	KeSetEvent(&(gKLogger->StartEvent), 0, FALSE);

	PVOID handles[2];
	handles[0] = (PVOID)&(gKLogger->FlushEvent);
	handles[1] = (PVOID)&(gKLogger->StopEvent);

	LARGE_INTEGER Timeout;
	Timeout.QuadPart = -FLUSH_TIMEOUT;

	NTSTATUS Status, WriteStatus;
	SIZE_T Length = 0;
	while (TRUE) {
		Status = KeWaitForMultipleObjects(
			2,
			handles,
			WaitAny,
			Executive,
			KernelMode,
			TRUE,
			&Timeout,
			NULL);

		if (Status == STATUS_TIMEOUT)
			DbgPrint("Flushing thread is woken by TIMEOUT\n");

		if (Status == STATUS_WAIT_0)
			DbgPrint("Flushing thread is woken by FLUSH EVENT\n");			

		if (Status == STATUS_TIMEOUT || Status == STATUS_WAIT_0) {
			Length = FLUSH_BUF_SIZE;

			int Err = RBRead(gKLogger->pRingBuf, gKLogger->pFlushingBuf, &Length);
			if (Err == ERROR_SUCCESS) {
				WriteStatus = WriteToFile(gKLogger->FileHandle, gKLogger->pFlushingBuf, Length);
				if (WriteStatus != STATUS_SUCCESS) {
					DbgPrint("Error: can't write to log file, return code %d\n", WriteStatus);
				}

			} else {
				DbgPrint("Error: can't read from ring_buffer, return code %d\n", Err);
			}

		} else if (Status == STATUS_WAIT_1) {
			KeClearEvent(&gKLogger->StopEvent);
			PsTerminateSystemThread(ERROR_SUCCESS); // exit
		}

		if (Status == STATUS_WAIT_0) {
			KeClearEvent(&gKLogger->FlushEvent);
			if (!InterlockedExchange(&(gKLogger->IsFlushDispatched), 0))
				__debugbreak();
		}
	}
}

SIZE_T GetRingBufSize(
	PUNICODE_STRING RegistryPath
) {  
    HANDLE RegKeyHandle;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES OdjAttr;
    UNICODE_STRING RegKeyPath;
    ULONG KeyValue = DEFAULT_RING_BUF_SIZE;
 
    PKEY_VALUE_PARTIAL_INFORMATION PartInfo;
    ULONG PartInfoSize;
 
    InitializeObjectAttributes(&OdjAttr, RegistryPath, 0, NULL, NULL);
 
    Status = ZwCreateKey(
		&RegKeyHandle, 
		KEY_QUERY_VALUE | KEY_SET_VALUE, 
		&OdjAttr, 
		0,  
		NULL, 
		REG_OPTION_NON_VOLATILE, 
		NULL
	);

    if (!NT_SUCCESS(Status)) {
        DbgPrint("[library_driver]: 'ZwCreateKey()' failed");
        return DEFAULT_RING_BUF_SIZE;
    }
 
    RtlInitUnicodeString(&RegKeyPath, REGISTRY_BUF_SIZE_KEY);
   
    PartInfoSize = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(PartInfoSize);
    PartInfo = ExAllocatePool(PagedPool, PartInfoSize);
    if (!PartInfo) {
        DbgPrint("[library_driver]: 'ExAllocatePool()' failed");
        ZwClose(RegKeyHandle);
        return DEFAULT_RING_BUF_SIZE;
    }
 
    Status = ZwQueryValueKey(RegKeyHandle, &RegKeyPath, KeyValuePartialInformation,
        PartInfo, PartInfoSize, &PartInfoSize);
 
    switch (Status) {
        case STATUS_SUCCESS:
            DbgPrint("[library_driver]: switch: STATUS_SUCCESS");
            if (PartInfo->Type == REG_DWORD && PartInfo->DataLength == sizeof(ULONG)) {
                RtlCopyMemory(&KeyValue, PartInfo->Data, sizeof(KeyValue));
                ZwClose(RegKeyHandle);
                ExFreePool(PartInfo);
                return KeyValue;
            }
            // break; - not break
 
        case STATUS_OBJECT_NAME_NOT_FOUND:
            DbgPrint("[library_driver]: switch: STATUS_OBJECT_NAME_NOT_FOUND");
            Status = ZwSetValueKey(RegKeyHandle, &RegKeyPath, 0, REG_DWORD, &KeyValue, sizeof(KeyValue));
            if (!NT_SUCCESS(Status)) {
                ZwClose(RegKeyHandle);
                ExFreePool(PartInfo);
                return DEFAULT_RING_BUF_SIZE;
            }
 
            break;
 
        default:
            DbgPrint("[library_driver]: switch: default");
            ZwClose(RegKeyHandle);
            ExFreePool(PartInfo);
            return DEFAULT_RING_BUF_SIZE;
 
            break;
           
    }
 
    ZwClose(RegKeyHandle);
    ExFreePool(PartInfo);
 
    return DEFAULT_RING_BUF_SIZE;
}

INT 
KLoggerInit(
	PUNICODE_STRING RegistryPath
) {
	int Err = ERROR_SUCCESS;

	gKLogger = (PKLOGGER)ExAllocatePool(NonPagedPool, sizeof(KLOGGER));
	if (gKLogger == NULL) {
		Err = ERROR_NOT_ENOUGH_MEMORY;
		goto err_klogger_mem;
	}

	SIZE_T RingBufSize = GetRingBufSize(RegistryPath);
	Err = RBInit(&(gKLogger->pRingBuf), RingBufSize);

	if (Err != ERROR_SUCCESS) {
		goto err_ring_buf_init;
	}

	KeInitializeEvent(&(gKLogger->FlushEvent), SynchronizationEvent, FALSE);
	KeInitializeEvent(&(gKLogger->StartEvent), SynchronizationEvent, FALSE);
	KeInitializeEvent(&(gKLogger->StopEvent), SynchronizationEvent, FALSE);

	gKLogger->IsFlushDispatched = 0;
	gKLogger->pFlushDpc = (PKDPC)ExAllocatePool(NonPagedPool, sizeof(KDPC));
	if (!gKLogger->pFlushDpc) {
		Err = ERROR_NOT_ENOUGH_MEMORY;
		goto err_dpc_mem;
	}

	KeInitializeDpc(gKLogger->pFlushDpc, SetWriteEvent, NULL);

	// alloc buffer for flushing thread
	gKLogger->pFlushingBuf = (PCHAR)ExAllocatePool(PagedPool, FLUSH_BUF_SIZE * sizeof(CHAR));
	if (!gKLogger->pFlushingBuf) {
		Err = ERROR_NOT_ENOUGH_MEMORY;
		goto err_flush_mem;
	}

	// open file for flushing thread
	UNICODE_STRING UniName;
	OBJECT_ATTRIBUTES ObjAttr;
	RtlInitUnicodeString(&UniName, LOG_FILE_NAME);
	IO_STATUS_BLOCK IoStatusBlock;

	InitializeObjectAttributes(
		&ObjAttr,
		(PUNICODE_STRING)&UniName,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
		NULL,
		NULL
	);

	NTSTATUS Status = ZwCreateFile(
		&(gKLogger->FileHandle),
		FILE_APPEND_DATA,
		&ObjAttr,
		&IoStatusBlock,
		0,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_WRITE,
		FILE_OPEN_IF,
		FILE_SYNCHRONOUS_IO_ALERT,
		NULL,
		0
	);

	if (!NT_SUCCESS(Status)) {
		Err = ERROR_CANNOT_MAKE;
		goto err_file;
	}

	Status = PsCreateSystemThread(
		&(gKLogger->FlushingThreadHandle),
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		FlushingThreadFunc,
		NULL);

	if (NT_SUCCESS(Status)) {
		Status = ObReferenceObjectByHandle(
			gKLogger->FlushingThreadHandle,
			FILE_ANY_ACCESS,
			NULL,
			KernelMode,
			(PVOID*)&(gKLogger->pFlushingThread),
			NULL);

	} else {
		Err = ERROR_TOO_MANY_TCBS;
		goto err_thread;
	}

	// wait while thread start
	LARGE_INTEGER Timeout;
	Timeout.QuadPart = -START_TIMEOUT;

	KeWaitForSingleObject(
		&(gKLogger->StartEvent),
		Executive,
		KernelMode,
		FALSE,
		&Timeout);

	return STATUS_SUCCESS;

err_thread:
	ZwClose(gKLogger->FileHandle);

err_file:
	ExFreePool(gKLogger->pFlushingBuf);

err_flush_mem:
	ExFreePool(gKLogger->pFlushDpc);

err_dpc_mem:
	RBDeinit(gKLogger->pRingBuf);

err_ring_buf_init:
	ExFreePool(gKLogger);

err_klogger_mem:
	return Err;
}

VOID 
KLoggerDeinit() {
	KeFlushQueuedDpcs();

	KeSetEvent(&(gKLogger->StopEvent), 0, FALSE);

	KeWaitForSingleObject(
		gKLogger->pFlushingThread,
		Executive,
		KernelMode,
		FALSE,
		NULL);

	ObDereferenceObject(gKLogger->pFlushingThread);
	ZwClose(gKLogger->FlushingThreadHandle);

	ExFreePool(gKLogger->pFlushingBuf);
	ZwClose(gKLogger->FileHandle);

	ExFreePool(gKLogger->pFlushDpc);

	RBDeinit(gKLogger->pRingBuf);
	ExFreePool(gKLogger);
}

static SIZE_T 
StrLen(
	PCSTR Str
) {
	SIZE_T Length = 0;
	while (*(Str + Length) != '\0') {
		Length++;
	}

	return Length;
}

VOID 
SetWriteEvent(
	IN PKDPC pthisDpcObject,
	IN PVOID DeferredContext,
	IN PVOID SystemArgument1,
	IN PVOID SystemArgument2
)
{
	UNREFERENCED_PARAMETER(pthisDpcObject);
	UNREFERENCED_PARAMETER(DeferredContext);
	UNREFERENCED_PARAMETER(SystemArgument1);
	UNREFERENCED_PARAMETER(SystemArgument2);

	DbgPrint("Set Write Event\n");
	KeSetEvent(&gKLogger->FlushEvent, 0, FALSE);
}

INT 
KLoggerLog(
	PCSTR LogMsg
) {
	int Err = RBWrite(gKLogger->pRingBuf, LogMsg, StrLen(LogMsg));
	int LoadFactor = RBLoadFactor(gKLogger->pRingBuf);
	DbgPrint("Load factor: %d\n", LoadFactor);
	LONG OrigDst;

	if (((LoadFactor >= FLUSH_THRESHOLD) || (Err == ERROR_INSUFFICIENT_BUFFER))) {
		DbgPrint("Pre Interlocked: is flush dpc queued: %d", gKLogger->IsFlushDispatched);
		OrigDst = InterlockedCompareExchange(&(gKLogger->IsFlushDispatched), 1, 0);
		DbgPrint("Post Interlocked original value: %d, is flush dpc queued: %d", OrigDst, gKLogger->IsFlushDispatched);
		if (!OrigDst) {
			DbgPrint("Dpc is queued, load factor: %d\n", LoadFactor);
			KeInsertQueueDpc(gKLogger->pFlushDpc, NULL, NULL);
		}
	}
	return Err;
}