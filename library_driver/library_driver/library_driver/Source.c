#include <ntddk.h>
#include <winerror.h>
#include "KLogger.h"


VOID DriverUnload(
	_In_ struct _DRIVER_OBJECT *DriverObject
)
{
	UNREFERENCED_PARAMETER(DriverObject);
	// __debugbreak();
	DbgPrint("[library_driver]: 'DriverUnload()' is executed");
	return;
}

NTSTATUS DriverEntry(
	_In_ struct _DRIVER_OBJECT *DriverObject,
	_In_ PUNICODE_STRING       RegistryPath
)
{
	// __debugbreak();
	DbgPrint("[library_driver]: 'DriverEntry()' is executed");
	UNREFERENCED_PARAMETER(RegistryPath);
	DriverObject->DriverUnload = DriverUnload;
	return STATUS_SUCCESS;
}

NTSTATUS DllInitialize(
	_In_ PUNICODE_STRING RegistryPath
)
{
	//UNREFERENCED_PARAMETER(RegistryPath);
	DbgPrint("[library_driver]: 'DllInitialize()' is started");

	INT err = KLoggerInit(RegistryPath);
	if (err != ERROR_SUCCESS)
	{
		DbgPrint("[klogger_test]: DriverEntry(): 'KLoggerInit()' returned err = %d", err);
		return err;
	}

	DbgPrint("[library_driver]: 'DllInitialize()' is finished");
	return STATUS_SUCCESS;
}

NTSTATUS DllUnload(void)
{
	DbgPrint("[library_driver]: 'DllUnload()' is started");

	KLoggerDeinit();

	DbgPrint("[library_driver]: 'DllUnload()' is finished");
	return STATUS_SUCCESS;
}