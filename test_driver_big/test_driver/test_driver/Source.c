#include <ntddk.h>
#include <ntdef.h>
#include <WinError.h>

#include "KLogger_lib.h"


HANDLE ThreadHandle;
PKTHREAD pThread;

NTSTATUS DriverEntry(
	_In_ struct _DRIVER_OBJECT *DriverObject, 
	_In_ PUNICODE_STRING RegistryPath
);

VOID DriverUnload(
	_In_ struct _DRIVER_OBJECT *DriverObject
);


VOID ThreadFunc(
	IN PVOID _Unused
) {
	UNREFERENCED_PARAMETER(_Unused);

	PCHAR Message[] = {
	"[klogtest 1]: curIRQL == 0\r\n",
	"[klogtest 1]: curIRQL == 1\r\n",
	"[klogtest 1]: curIRQL == 2\r\n",
	"[klogtest 1]: curIRQL == 3\r\n",
	"[klogtest 1]: curIRQL == 4\r\n",
	"[klogtest 1]: curIRQL == 5\r\n",
	"[klogtest 1]: curIRQL == 6\r\n",
	"[klogtest 1]: curIRQL == 7\r\n",
	"[klogtest 1]: curIRQL == 8\r\n",
	"[klogtest 1]: curIRQL == 9\r\n",
	"[klogtest 1]: curIRQL == 10\r\n",
	"[klogtest 1]: curIRQL == 11\r\n",
	"[klogtest 1]: curIRQL == 12\r\n",
	"[klogtest 1]: curIRQL == 13\r\n",
	"[klogtest 1]: curIRQL == 14\r\n",
	"[klogtest 1]: curIRQL == 15\r\n"
	};

#define LOG_REPETITIONS_COUNT 1000000
	DbgPrint("1 Fast path: DPC flushing first part of messages");
	KIRQL StartIrql = KeGetCurrentIrql();
	for (KIRQL curIrql = StartIrql; curIrql <= HIGH_LEVEL; ++curIrql) {
		KIRQL _OldIrql;
		KeRaiseIrql(curIrql, &_OldIrql);

		for (INT i = 0; i < LOG_REPETITIONS_COUNT; ++i) {
			INT LogStat = KLoggerLog(Message[curIrql]);
			DbgPrint("[klogtest 1]: curIRQL == %d, message: %s, status: %d", 
				curIrql, 
				Message[curIrql], 
				LogStat
			);
		}

		KeLowerIrql(StartIrql);
	}

	LARGE_INTEGER DueTime;
	DueTime.QuadPart = -10000000LL;	// 10^7 * 100us = 1; relative value
	LARGE_INTEGER	Interval;

#define FLUSH_TIMEOUT 10000000ll
	Interval.QuadPart = -2 * FLUSH_TIMEOUT;

	KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	DbgPrint("1 ...");
	DbgPrint("1 ...");
	DbgPrint("1 Slow path: TIMEOUT flushing all messages");

	for (KIRQL curIrql = StartIrql; curIrql <= HIGH_LEVEL; ++curIrql) {
		KIRQL _OldIrql;
		KeRaiseIrql(curIrql, &_OldIrql);
		
		for (INT i = 0; i < LOG_REPETITIONS_COUNT; ++i) {
			INT LogStat = KLoggerLog(Message[curIrql]);
			DbgPrint("[klogtest 1]: curIRQL == %d, message: %s, status: %d", 
				curIrql, 
				Message[curIrql], 
				LogStat
			);
		}

		KeLowerIrql(StartIrql);
		KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	}

	KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	DbgPrint("1 ...");
	DbgPrint("1 ...");
	DbgPrint("1 Combined path: DPC (1.9 msg) and TIMEOUT (1.1 msg) flushing messages");

	for (KIRQL curIrql = StartIrql; curIrql <= HIGH_LEVEL; ++curIrql) {
		KIRQL _OldIrql;
		KeRaiseIrql(curIrql, &_OldIrql);
		
		for (INT i = 0; i < LOG_REPETITIONS_COUNT; ++i) {
			INT LogStat = KLoggerLog(Message[curIrql]);
			DbgPrint("[klogtest 1]: curIRQL == %d, message: %s, status: %d", 
				curIrql, 
				Message[curIrql], 
				LogStat
			);
		}

		KeLowerIrql(StartIrql);
		if (curIrql % 3 == 0)
			KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	}

	PsTerminateSystemThread(ERROR_SUCCESS);
}


NTSTATUS 
DriverEntry(
	_In_ struct _DRIVER_OBJECT *DriverObject,
	_In_ PUNICODE_STRING       RegistryPath
) {
	UNREFERENCED_PARAMETER(RegistryPath);
	// __debugbreak();
	DbgPrint("[test_driver_stress]: 'DriverEntry()' is executed");
	DriverObject->DriverUnload = DriverUnload;


	DbgPrint("[test_driver_stress]: 'PsCreateSystemThread()' is started");
	NTSTATUS status = PsCreateSystemThread(
		&ThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		ThreadFunc,
		NULL);
	DbgPrint("[test_driver_stress]: 'PsCreateSystemThread()' is finished");

	if (NT_SUCCESS(status)) {
		DbgPrint("[test_driver_stress]: 'ObReferenceObjectByHandle()' is started");
		status = ObReferenceObjectByHandle(
			ThreadHandle,
			FILE_ANY_ACCESS,
			NULL,
			KernelMode,
			(PVOID *) &(pThread),
			NULL);
		DbgPrint("[test_driver_stress]: 'ObReferenceObjectByHandle()' is finished, status %d", status);

	} else {
		DbgPrint("[test_driver_stress]: error exit");
		return ERROR_TOO_MANY_TCBS;
	}

	DbgPrint("[klogger_test_stress]: 'DriverEntry()' finished");
	return STATUS_SUCCESS;
}

VOID 
DriverUnload(
	_In_ struct _DRIVER_OBJECT *DriverObject
) {
	UNREFERENCED_PARAMETER(DriverObject);
	DbgPrint("[test_driver_1]: 'DriverUnload()' is started");

	DbgPrint("[test_driver_1]: 'KeWaitForSingleObject()' is started");
	KeWaitForSingleObject(
		pThread,
		Executive,
		KernelMode,
		FALSE,
		NULL
	);

	DbgPrint("[test_driver_1]: 'KeWaitForSingleObject()' is finished");

	ObDereferenceObject(pThread);
	ZwClose(ThreadHandle);

	DbgPrint("[test_driver_1]: 'DriverUnload()' is finished");
	return;
}