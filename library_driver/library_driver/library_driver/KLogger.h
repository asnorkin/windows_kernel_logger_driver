#pragma once

#include <ntddk.h>

#define LOG_FILE_NAME L"\\??\\C:\\klogger.log"

typedef struct KLogger* PKLOGGER;

INT KLoggerInit(PUNICODE_STRING RegistryPath);
VOID KLoggerDeinit();
INT KLoggerLog(PCSTR log_msg);
