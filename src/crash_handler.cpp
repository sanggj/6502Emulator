#include "crash_handler.hpp"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

namespace {

LONG CALLBACK crashFilter(EXCEPTION_POINTERS* info) {
    // Write a minidump next to the executable before terminating.
    char path[MAX_PATH * 2]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    else path[0] = '\0';
    char file[MAX_PATH * 2]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(file, sizeof(file), "%scrash_%04u%02u%02u_%02u%02u%02u.dmp", path,
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE dumpFile = CreateFileA(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
        HMODULE dbgHelp = LoadLibraryA("dbghelp.dll");
        if (dbgHelp) {
            using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE,
                                                      MINIDUMP_TYPE,
                                                      PMINIDUMP_EXCEPTION_INFORMATION,
                                                      PMINIDUMP_USER_STREAM_INFORMATION,
                                                      PMINIDUMP_CALLBACK_INFORMATION);
            auto writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                GetProcAddress(dbgHelp, "MiniDumpWriteDump"));
            if (writeDump)
                writeDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
                          MiniDumpNormal, &mei, nullptr, nullptr);
            FreeLibrary(dbgHelp);
        }
        CloseHandle(dumpFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void installCrashHandler() { SetUnhandledExceptionFilter(crashFilter); }

#else

void installCrashHandler() {}

#endif
