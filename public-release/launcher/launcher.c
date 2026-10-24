#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#ifndef QBTC_ENTRY_SCRIPT
#define QBTC_ENTRY_SCRIPT L"START.ps1"
#endif

static int fail(const wchar_t *message) {
    MessageBoxW(NULL, message, L"QBTC", MB_OK | MB_ICONERROR);
    return 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)instance; (void)previous; (void)args; (void)show;
    wchar_t directory[32768], script[32768], powershell[32768], command[32768];
    DWORD length = GetModuleFileNameW(NULL, directory, 32768);
    if (!length || length >= 32768) return fail(L"Unable to locate QBTC. Extract the complete ZIP into a folder first.");
    wchar_t *slash = wcsrchr(directory, L'\\');
    if (!slash) return fail(L"Unable to locate the QBTC folder.");
    *slash = L'\0';
    if (swprintf(script, 32768, L"%ls\\app\\%ls", directory, QBTC_ENTRY_SCRIPT) < 0)
        return fail(L"The QBTC folder path is too long.");
    DWORD attributes = GetFileAttributesW(script);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return fail(L"Extract the complete ZIP before opening QBTC.exe. Keep the app folder next to QBTC.exe.");
    UINT count = GetSystemDirectoryW(powershell, 32768);
    const wchar_t *suffix = L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (!count || count >= 32768 || count + wcslen(suffix) >= 32768)
        return fail(L"Unable to find Windows PowerShell.");
    wcscat(powershell, suffix);
    if (swprintf(command, 32768,
        L"\"%ls\" -NoProfile -STA -ExecutionPolicy Bypass -File \"%ls\"", powershell, script) < 0)
        return fail(L"The QBTC folder path is too long. Move the folder to a shorter path.");
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    // Hide the console only; do not suppress the first GUI window.
    if (!CreateProcessW(powershell, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, directory, &startup, &process))
        return fail(L"QBTC could not open Windows PowerShell. Check that the complete ZIP was extracted and Windows allows the program to run.");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code == 32) return 1; // START.ps1 already showed the detailed error.
    if (code) return fail(L"The QBTC panel closed with an error. Keep the complete app folder next to QBTC.exe and try again.");
    return 0;
}
