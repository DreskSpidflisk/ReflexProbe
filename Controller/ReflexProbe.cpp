#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include <float.h>

#include "../Common/Protocol.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "bcrypt.lib")

#define RP_WIDEN_INNER(x) L##x
#define RP_WIDEN(x) RP_WIDEN_INNER(x)

namespace {

constexpr wchar_t kWindowClass[] = L"ReflexProbeControlWindow";
constexpr wchar_t kControllerBuildStamp[] = RP_WIDEN(__DATE__) L" " RP_WIDEN(__TIME__);
constexpr UINT_PTR kPollTimer = 1;
constexpr int kMargin = 12;
constexpr int kStatusTop = 186;
constexpr int kMinimumWindowWidth = 700;
constexpr int kMinimumWindowHeight = 480;
constexpr WPARAM kStatusTextLimit = 16u * 1024u * 1024u;

enum ControlId : int {
    IDC_GAME_PATH = 1001,
    IDC_BROWSE,
    IDC_ARGUMENTS,
    IDC_OVERRIDE_ENABLE,
    IDC_OVERRIDE_FPS,
    IDC_APPLY_OVERRIDE,
    IDC_LAUNCH,
    IDC_STATUS,
    IDC_WORD_WRAP
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;

    HWND gameLabel = nullptr;
    HWND gamePath = nullptr;
    HWND browse = nullptr;
    HWND argumentsLabel = nullptr;
    HWND arguments = nullptr;
    HWND overrideEnable = nullptr;
    HWND overrideFps = nullptr;
    HWND fpsLabel = nullptr;
    HWND applyOverride = nullptr;
    HWND launch = nullptr;
    HWND statusLabel = nullptr;
    HWND wordWrap = nullptr;
    HWND status = nullptr;
    WNDPROC statusOriginalProc = nullptr;

    HANDLE process = nullptr;
    DWORD processId = 0;
    HANDLE mapping = nullptr;
    ReflexProbeProtocol::SharedState* shared = nullptr;

    LONG lastHookState = -1;
    LONG lastEventSerial = 0;
    bool loggedInjectedBuild = false;
};

AppState g_app;

void LayoutControls(int clientWidth, int clientHeight);

void SetChildFont(HWND child, HFONT font)
{
    if (child)
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void AppendStatus(const wchar_t* text)
{
    if (!g_app.status || !text)
        return;

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    const bool preserveSelection = GetFocus() == g_app.status;
    if (preserveSelection) {
        SendMessageW(g_app.status, EM_GETSEL,
            reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
    }

    SendMessageW(g_app.status, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(g_app.status, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));

    if (preserveSelection) {
        SendMessageW(g_app.status, EM_SETSEL,
            static_cast<WPARAM>(selectionStart), static_cast<LPARAM>(selectionEnd));
    } else {
        SendMessageW(g_app.status, EM_SCROLLCARET, 0, 0);
    }
}

void AppendStatusCount(const wchar_t* text, size_t count)
{
    if (!text || !count)
        return;

    wchar_t chunk[1024]{};
    while (count) {
        const size_t copyCount = count < (_countof(chunk) - 1) ? count : (_countof(chunk) - 1);
        wmemcpy(chunk, text, copyCount);
        chunk[copyCount] = 0;
        AppendStatus(chunk);
        text += copyCount;
        count -= copyCount;
    }
}

void FormatTimestamp(LONGLONG eventQpc, wchar_t* out, size_t outCount)
{
    if (!out || !outCount)
        return;

    SYSTEMTIME localTime{};
    bool converted = false;

    if (eventQpc > 0) {
        LARGE_INTEGER nowQpc{};
        LARGE_INTEGER frequency{};
        FILETIME nowUtc{};
        if (QueryPerformanceCounter(&nowQpc) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            GetSystemTimeAsFileTime(&nowUtc);

            ULARGE_INTEGER eventUtc{};
            eventUtc.LowPart = nowUtc.dwLowDateTime;
            eventUtc.HighPart = nowUtc.dwHighDateTime;

            if (eventQpc < nowQpc.QuadPart) {
                const LONGLONG deltaQpc = nowQpc.QuadPart - eventQpc;
                const double delta100nsDouble =
                    (static_cast<double>(deltaQpc) * 10000000.0) /
                    static_cast<double>(frequency.QuadPart);
                const ULONGLONG delta100ns = static_cast<ULONGLONG>(delta100nsDouble);
                if (delta100ns < eventUtc.QuadPart)
                    eventUtc.QuadPart -= delta100ns;
            }

            FILETIME eventUtcFile{};
            eventUtcFile.dwLowDateTime = eventUtc.LowPart;
            eventUtcFile.dwHighDateTime = eventUtc.HighPart;
            FILETIME eventLocalFile{};
            if (FileTimeToLocalFileTime(&eventUtcFile, &eventLocalFile) &&
                FileTimeToSystemTime(&eventLocalFile, &localTime)) {
                converted = true;
            }
        }
    }

    if (!converted)
        GetLocalTime(&localTime);

    swprintf_s(out, outCount, L"[%02u:%02u:%02u] ",
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond));
}

void AppendStatusLineAtQpc(const wchar_t* text, LONGLONG eventQpc)
{
    if (!text)
        return;

    const wchar_t* cursor = text;
    for (;;) {
        const wchar_t* newline = wcsstr(cursor, L"\r\n");
        const size_t lineLength = newline ? static_cast<size_t>(newline - cursor) : wcslen(cursor);

        wchar_t timestamp[32]{};
        FormatTimestamp(eventQpc, timestamp, _countof(timestamp));
        AppendStatus(timestamp);
        AppendStatusCount(cursor, lineLength);
        AppendStatus(L"\r\n");

        if (!newline)
            break;
        cursor = newline + 2;
    }
}

void AppendStatusLine(const wchar_t* text)
{
    AppendStatusLineAtQpc(text, 0);
}

LRESULT CALLBACK StatusEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN && wParam == static_cast<WPARAM>('A') &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        SendMessageW(window, EM_SETSEL, 0, static_cast<LPARAM>(-1));
        return 0;
    }

    if (g_app.statusOriginalProc)
        return CallWindowProcW(g_app.statusOriginalProc, window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

void RecreateStatusControl(bool wordWrap)
{
    if (!g_app.window)
        return;

    wchar_t* savedText = nullptr;
    int savedLength = 0;
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    int firstVisibleLine = 0;

    if (g_app.status) {
        savedLength = GetWindowTextLengthW(g_app.status);
        if (savedLength > 0) {
            const SIZE_T bytes = (static_cast<SIZE_T>(savedLength) + 1) * sizeof(wchar_t);
            savedText = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, bytes));
            if (savedText)
                GetWindowTextW(g_app.status, savedText, savedLength + 1);
        }

        SendMessageW(g_app.status, EM_GETSEL,
            reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
        firstVisibleLine = static_cast<int>(SendMessageW(g_app.status, EM_GETFIRSTVISIBLELINE, 0, 0));

        if (g_app.statusOriginalProc) {
            SetWindowLongPtrW(g_app.status, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_app.statusOriginalProc));
            g_app.statusOriginalProc = nullptr;
        }
        DestroyWindow(g_app.status);
        g_app.status = nullptr;
    }

    DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
        ES_READONLY | ES_NOHIDESEL | WS_VSCROLL;
    if (!wordWrap)
        style |= WS_HSCROLL | ES_AUTOHSCROLL;

    g_app.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        style, 0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(IDC_STATUS), g_app.instance, nullptr);
    if (!g_app.status) {
        if (savedText)
            HeapFree(GetProcessHeap(), 0, savedText);
        return;
    }

    SetChildFont(g_app.status, static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT)));
    SendMessageW(g_app.status, EM_SETLIMITTEXT, kStatusTextLimit, 0);
    g_app.statusOriginalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_app.status, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&StatusEditProc)));

    if (savedText) {
        SetWindowTextW(g_app.status, savedText);
        SendMessageW(g_app.status, EM_SETSEL,
            static_cast<WPARAM>(selectionStart), static_cast<LPARAM>(selectionEnd));
        const int currentFirstLine = static_cast<int>(SendMessageW(g_app.status, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(g_app.status, EM_LINESCROLL, 0,
            static_cast<LPARAM>(firstVisibleLine - currentFirstLine));
        HeapFree(GetProcessHeap(), 0, savedText);
    }

    RECT client{};
    GetClientRect(g_app.window, &client);
    LayoutControls(client.right - client.left, client.bottom - client.top);
}

void BuildMappingName(DWORD processId, wchar_t* out, size_t outCount)
{
    swprintf_s(out, outCount, L"%s%lu", ReflexProbeProtocol::kMappingPrefix, processId);
}

const wchar_t* PathFileName(const wchar_t* path)
{
    if (!path)
        return L"";

    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* slash2 = wcsrchr(path, L'/');
    if (!slash || (slash2 && slash2 > slash))
        slash = slash2;
    return slash ? slash + 1 : path;
}

void PathDirectory(const wchar_t* path, wchar_t* out, size_t outCount)
{
    if (!out || !outCount)
        return;

    out[0] = 0;
    if (!path || !*path)
        return;

    wcscpy_s(out, outCount, path);
    wchar_t* slash = wcsrchr(out, L'\\');
    wchar_t* slash2 = wcsrchr(out, L'/');
    if (!slash || (slash2 && slash2 > slash))
        slash = slash2;
    if (slash)
        *slash = 0;
}

bool GetSiblingDllPath(wchar_t* out, size_t outCount)
{
    if (!out || !outCount)
        return false;

    DWORD count = GetModuleFileNameW(nullptr, out, static_cast<DWORD>(outCount));
    if (!count || count >= outCount)
        return false;

    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash)
        return false;

    slash[1] = 0;
    return wcscat_s(out, outCount, L"ReflexProbe64.dll") == 0;
}

bool BCryptSucceeded(NTSTATUS status)
{
    return status >= 0;
}

bool HashFileSha256(const wchar_t* path, wchar_t* out, size_t outCount)
{
    if (!path || !out || outCount < 65)
        return false;
    out[0] = 0;

    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    PUCHAR hashObject = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD propertyBytes = 0;
    bool success = false;

    if (!BCryptSucceeded(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        goto Cleanup;
    }

    if (!BCryptSucceeded(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), static_cast<ULONG>(sizeof(objectBytes)),
            &propertyBytes, 0))) {
        goto Cleanup;
    }

    if (!BCryptSucceeded(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashBytes), static_cast<ULONG>(sizeof(hashBytes)),
            &propertyBytes, 0)) || hashBytes != 32) {
        goto Cleanup;
    }

    hashObject = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, objectBytes));
    if (!hashObject)
        goto Cleanup;

    if (!BCryptSucceeded(BCryptCreateHash(
            algorithm, &hash, hashObject, objectBytes, nullptr, 0, 0))) {
        goto Cleanup;
    }

    {
        BYTE buffer[64 * 1024]{};
        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr))
                goto Cleanup;
            if (!bytesRead)
                break;
            if (!BCryptSucceeded(BCryptHashData(hash, buffer, bytesRead, 0)))
                goto Cleanup;
        }
    }

    {
        BYTE digest[32]{};
        if (!BCryptSucceeded(BCryptFinishHash(hash, digest, static_cast<ULONG>(sizeof(digest)), 0)))
            goto Cleanup;

        for (size_t i = 0; i < _countof(digest); ++i) {
            swprintf_s(out + (i * 2), outCount - (i * 2), L"%02x",
                static_cast<unsigned int>(digest[i]));
        }
        success = true;
    }

Cleanup:
    if (hash)
        BCryptDestroyHash(hash);
    if (hashObject)
        HeapFree(GetProcessHeap(), 0, hashObject);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!success)
        out[0] = 0;
    return success;
}

void LogBinaryIdentity()
{
    wchar_t line[1200]{};
    swprintf_s(line, L"Source tag: %s | protocol %u",
        ReflexProbeProtocol::kBuildTag, ReflexProbeProtocol::kVersion);
    AppendStatusLine(line);
    swprintf_s(line, L"Controller build: %s", kControllerBuildStamp);
    AppendStatusLine(line);

    wchar_t exePath[ReflexProbeProtocol::kPathChars]{};
    wchar_t dllPath[ReflexProbeProtocol::kPathChars]{};
    wchar_t hash[65]{};

    const DWORD exeChars = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath)));
    if (exeChars && exeChars < _countof(exePath) && HashFileSha256(exePath, hash, _countof(hash))) {
        swprintf_s(line, L"ReflexProbe.exe SHA-256: %s", hash);
        AppendStatusLine(line);
    } else {
        AppendStatusLine(L"ReflexProbe.exe SHA-256: unavailable");
    }

    if (GetSiblingDllPath(dllPath, _countof(dllPath)) && HashFileSha256(dllPath, hash, _countof(hash))) {
        swprintf_s(line, L"ReflexProbe64.dll SHA-256: %s", hash);
        AppendStatusLine(line);
    } else {
        AppendStatusLine(L"ReflexProbe64.dll SHA-256: unavailable (build/copy the injected project beside the controller)");
    }
}

uintptr_t FindRemoteModuleBase(DWORD processId, const wchar_t* moduleName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uintptr_t result = 0;

    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

bool GetLocalFunctionOwner(void* function, HMODULE& owner, wchar_t* moduleName, size_t moduleNameCount)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(function, &memory, sizeof(memory)))
        return false;

    owner = reinterpret_cast<HMODULE>(memory.AllocationBase);

    wchar_t fullPath[MAX_PATH]{};
    DWORD count = GetModuleFileNameW(owner, fullPath, static_cast<DWORD>(_countof(fullPath)));
    if (!count || count >= _countof(fullPath))
        return false;

    return wcscpy_s(moduleName, moduleNameCount, PathFileName(fullPath)) == 0;
}

bool WaitForRemoteFunctionOwner(DWORD processId, const wchar_t* moduleName, uintptr_t& baseOut, DWORD timeoutMs)
{
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    do {
        baseOut = FindRemoteModuleBase(processId, moduleName);
        if (baseOut)
            return true;
        Sleep(1);
    } while (GetTickCount64() < end);

    return false;
}

bool InjectDll(HANDLE process, DWORD processId, const wchar_t* dllPath, wchar_t* error, size_t errorCount)
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    if (!loadLibrary) {
        swprintf_s(error, errorCount, L"Could not resolve local LoadLibraryW.");
        return false;
    }

    HMODULE functionOwner = nullptr;
    wchar_t ownerName[MAX_PATH]{};
    if (!GetLocalFunctionOwner(reinterpret_cast<void*>(loadLibrary), functionOwner, ownerName, _countof(ownerName))) {
        swprintf_s(error, errorCount, L"Could not identify the module that owns LoadLibraryW.");
        return false;
    }

    uintptr_t remoteOwner = 0;
    if (!WaitForRemoteFunctionOwner(processId, ownerName, remoteOwner, 5000)) {
        swprintf_s(error, errorCount, L"Target never loaded %s, which owns LoadLibraryW on this system.", ownerName);
        return false;
    }

    const uintptr_t localOwner = reinterpret_cast<uintptr_t>(functionOwner);
    const uintptr_t localFunction = reinterpret_cast<uintptr_t>(loadLibrary);
    const uintptr_t remoteFunction = remoteOwner + (localFunction - localOwner);

    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) {
        swprintf_s(error, errorCount, L"VirtualAllocEx failed (%lu).", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath, bytes, &written) || written != bytes) {
        swprintf_s(error, errorCount, L"WriteProcessMemory failed (%lu).", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteFunction), remotePath, 0, nullptr);
    if (!thread) {
        swprintf_s(error, errorCount, L"CreateRemoteThread failed (%lu).", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 10000);
    CloseHandle(thread);

    if (wait != WAIT_OBJECT_0) {
        swprintf_s(error, errorCount, L"Remote LoadLibraryW did not finish in 10 seconds.");
        return false;
    }

    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    if (!FindRemoteModuleBase(processId, L"ReflexProbe64.dll")) {
        swprintf_s(error, errorCount, L"ReflexProbe64.dll was not present after LoadLibraryW returned.");
        return false;
    }

    return true;
}

bool ParseOverrideFromUi(bool& enabled, uint32_t& frameLimitUs, wchar_t* error, size_t errorCount)
{
    enabled = Button_GetCheck(g_app.overrideEnable) == BST_CHECKED;
    frameLimitUs = 0;

    if (!enabled)
        return true;

    wchar_t fpsText[64]{};
    GetWindowTextW(g_app.overrideFps, fpsText, static_cast<int>(_countof(fpsText)));

    wchar_t* end = nullptr;
    const double fps = wcstod(fpsText, &end);
    if (end == fpsText || !_finite(fps) || fps <= 0.0) {
        swprintf_s(error, errorCount, L"Override FPS must be a positive number.");
        return false;
    }

    double interval = 1000000.0 / fps;
    if (interval < 1.0)
        interval = 1.0;
    if (interval > 4294967295.0)
        interval = 4294967295.0;

    frameLimitUs = static_cast<uint32_t>(interval + 0.5);
    return true;
}

void ApplyOverrideToShared()
{
    if (!g_app.shared)
        return;

    bool enabled = false;
    uint32_t frameLimitUs = 0;
    wchar_t error[256]{};
    if (!ParseOverrideFromUi(enabled, frameLimitUs, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return;
    }

    InterlockedExchange(&g_app.shared->overrideUs, static_cast<LONG>(frameLimitUs));
    InterlockedExchange(&g_app.shared->overrideEnabled, enabled ? 1 : 0);
    InterlockedIncrement(&g_app.shared->configSequence);

    wchar_t line[256]{};
    if (enabled)
        swprintf_s(line, L"Override armed: %u us (%.3f FPS). Takes effect on the next Reflex settings call.",
            frameLimitUs, 1000000.0 / static_cast<double>(frameLimitUs));
    else
        wcscpy_s(line, L"Override disabled. Game Reflex options will pass through unchanged.");
    AppendStatusLine(line);
}

void CleanupTarget()
{
    if (g_app.shared) {
        UnmapViewOfFile(g_app.shared);
        g_app.shared = nullptr;
    }
    if (g_app.mapping) {
        CloseHandle(g_app.mapping);
        g_app.mapping = nullptr;
    }
    if (g_app.process) {
        CloseHandle(g_app.process);
        g_app.process = nullptr;
    }

    g_app.processId = 0;
    g_app.lastHookState = -1;
    g_app.lastEventSerial = 0;
    g_app.loggedInjectedBuild = false;
    if (g_app.launch)
        EnableWindow(g_app.launch, TRUE);
}

bool CreateSharedState(DWORD processId, const wchar_t* targetPath,
                       bool overrideEnabled, uint32_t overrideUs,
                       wchar_t* error, size_t errorCount)
{
    wchar_t mappingName[128]{};
    BuildMappingName(processId, mappingName, _countof(mappingName));

    g_app.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(ReflexProbeProtocol::SharedState), mappingName);
    if (!g_app.mapping) {
        swprintf_s(error, errorCount, L"CreateFileMapping failed (%lu).", GetLastError());
        return false;
    }

    g_app.shared = reinterpret_cast<ReflexProbeProtocol::SharedState*>(
        MapViewOfFile(g_app.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ReflexProbeProtocol::SharedState)));
    if (!g_app.shared) {
        swprintf_s(error, errorCount, L"MapViewOfFile failed (%lu).", GetLastError());
        CloseHandle(g_app.mapping);
        g_app.mapping = nullptr;
        return false;
    }

    ZeroMemory(g_app.shared, sizeof(*g_app.shared));
    g_app.shared->magic = ReflexProbeProtocol::kMagic;
    g_app.shared->version = ReflexProbeProtocol::kVersion;
    g_app.shared->structSize = sizeof(*g_app.shared);
    g_app.shared->targetProcessId = processId;
    g_app.shared->hookState = ReflexProbeProtocol::HookStateWaitingForDll;
    g_app.shared->backend = ReflexProbeProtocol::ReflexBackendUnknown;
    g_app.shared->overrideEnabled = overrideEnabled ? 1 : 0;
    g_app.shared->overrideUs = static_cast<LONG>(overrideUs);
    wcsncpy_s(g_app.shared->targetPath, _countof(g_app.shared->targetPath), targetPath, _TRUNCATE);
    return true;
}

bool LaunchAndInject()
{
    if (g_app.process)
        return false;

    wchar_t gamePath[ReflexProbeProtocol::kPathChars]{};
    wchar_t arguments[4096]{};
    GetWindowTextW(g_app.gamePath, gamePath, static_cast<int>(_countof(gamePath)));
    GetWindowTextW(g_app.arguments, arguments, static_cast<int>(_countof(arguments)));

    if (!gamePath[0]) {
        MessageBoxW(g_app.window, L"Choose a game executable first.", L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    DWORD attributes = GetFileAttributesW(gamePath);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_app.window, L"The selected game executable does not exist.", L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    bool overrideEnabled = false;
    uint32_t overrideUs = 0;
    wchar_t error[512]{};
    if (!ParseOverrideFromUi(overrideEnabled, overrideUs, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    wchar_t dllPath[MAX_PATH]{};
    if (!GetSiblingDllPath(dllPath, _countof(dllPath)) || GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_app.window,
            L"ReflexProbe64.dll was not found beside ReflexProbe.exe. Build both projects in the solution.",
            L"ReflexProbe", MB_ICONERROR);
        return false;
    }

    wchar_t workingDirectory[ReflexProbeProtocol::kPathChars]{};
    PathDirectory(gamePath, workingDirectory, _countof(workingDirectory));

    wchar_t commandLine[32768]{};
    if (arguments[0])
        swprintf_s(commandLine, L"\"%s\" %s", gamePath, arguments);
    else
        swprintf_s(commandLine, L"\"%s\"", gamePath);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(gamePath, commandLine, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, workingDirectory[0] ? workingDirectory : nullptr, &startup, &process)) {
        swprintf_s(error, L"CreateProcess failed (%lu).", GetLastError());
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONERROR);
        return false;
    }

    g_app.process = process.hProcess;
    g_app.processId = process.dwProcessId;

    BOOL targetIsWow64 = FALSE;
    if (IsWow64Process(process.hProcess, &targetIsWow64) && targetIsWow64) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        MessageBoxW(g_app.window, L"ReflexProbe bootstrap supports x64 targets only.", L"ReflexProbe", MB_ICONERROR);
        CleanupTarget();
        return false;
    }

    if (!CreateSharedState(process.dwProcessId, gamePath, overrideEnabled, overrideUs, error, _countof(error))) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONERROR);
        CleanupTarget();
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    HMODULE owner = nullptr;
    wchar_t ownerName[MAX_PATH]{};
    uintptr_t remoteOwner = 0;
    bool resumed = false;

    if (!loadLibrary || !GetLocalFunctionOwner(reinterpret_cast<void*>(loadLibrary), owner, ownerName, _countof(ownerName)) ||
        !WaitForRemoteFunctionOwner(process.dwProcessId, ownerName, remoteOwner, 50)) {
        ResumeThread(process.hThread);
        resumed = true;
    }

    if (!InjectDll(process.hProcess, process.dwProcessId, dllPath, error, _countof(error))) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        MessageBoxW(g_app.window, error, L"ReflexProbe injection failed", MB_ICONERROR);
        CleanupTarget();
        return false;
    }

    if (!resumed)
        ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    g_app.lastHookState = -1;
    g_app.lastEventSerial = 0;
    g_app.loggedInjectedBuild = false;
    EnableWindow(g_app.launch, FALSE);

    wchar_t line[512]{};
    swprintf_s(line, L"Launched %s (PID %lu) and injected ReflexProbe64.dll.", PathFileName(gamePath), process.dwProcessId);
    AppendStatusLine(line);
    if (overrideEnabled) {
        swprintf_s(line, L"Initial override: %u us (%.3f FPS).", overrideUs,
            1000000.0 / static_cast<double>(overrideUs));
        AppendStatusLine(line);
    } else {
        AppendStatusLine(L"Initial mode: observe only; game Reflex options pass through unchanged.");
    }

    return true;
}

const wchar_t* BackendName(LONG backend)
{
    switch (backend) {
    case ReflexProbeProtocol::ReflexBackendModernSetOptions:
        return L"Streamline 2.x+ slReflexSetOptions";
    case ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants:
        return L"Streamline 1.x slSetFeatureConstants";
    case ReflexProbeProtocol::ReflexBackendLegacyPluginConstants:
        return L"Streamline 1.x sl.reflex plugin gateway";
    default:
        return L"unknown";
    }
}

const wchar_t* BackendCallName(LONG backend)
{
    switch (backend) {
    case ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants:
        return L"slSetFeatureConstants";
    case ReflexProbeProtocol::ReflexBackendLegacyPluginConstants:
        return L"sl.reflex!slSetConstants";
    default:
        return L"slReflexSetOptions";
    }
}

void PollSharedState()
{
    if (!g_app.process || !g_app.shared)
        return;

    if (WaitForSingleObject(g_app.process, 0) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(g_app.process, &exitCode);
        wchar_t line[160]{};
        swprintf_s(line, L"Target exited with code %lu.", exitCode);
        AppendStatusLine(line);
        CleanupTarget();
        return;
    }

    if (!g_app.loggedInjectedBuild && g_app.shared->injectedBuild[0]) {
        wchar_t line[256]{};
        swprintf_s(line, L"Injected DLL reports build: %s", g_app.shared->injectedBuild);
        AppendStatusLine(line);
        g_app.loggedInjectedBuild = true;
    }

    const LONG backend = InterlockedCompareExchange(&g_app.shared->backend, 0, 0);
    const LONG hookState = InterlockedCompareExchange(&g_app.shared->hookState, 0, 0);
    if (hookState != g_app.lastHookState) {
        g_app.lastHookState = hookState;
        wchar_t line[2300]{};
        switch (hookState) {
        case ReflexProbeProtocol::HookStateWaitingForDll:
            wcscpy_s(line, L"Waiting for a Streamline Reflex interception path...");
            break;
        case ReflexProbeProtocol::HookStateInterceptArmed:
            swprintf_s(line, L"Interception armed: %s. Waiting for sl.reflex.dll to resolve its plugin gateway.",
                BackendName(backend));
            break;
        case ReflexProbeProtocol::HookStateReflexFound:
            swprintf_s(line, L"Found Reflex backend: %s\r\nModule: %s\r\nFile version: %s",
                BackendName(backend),
                g_app.shared->reflexPath[0] ? g_app.shared->reflexPath : L"(path unavailable)",
                g_app.shared->reflexVersion[0] ? g_app.shared->reflexVersion : L"unknown");
            break;
        case ReflexProbeProtocol::HookStateHooked:
            swprintf_s(line, L"Hook active: %s via %s\r\nModule: %s\r\nFile version: %s",
                BackendCallName(backend), BackendName(backend),
                g_app.shared->reflexPath[0] ? g_app.shared->reflexPath : L"sl.reflex.dll",
                g_app.shared->reflexVersion[0] ? g_app.shared->reflexVersion : L"unknown");
            break;
        case ReflexProbeProtocol::HookStateError:
            swprintf_s(line, L"Injected DLL error: %s",
                g_app.shared->lastError[0] ? g_app.shared->lastError : L"unknown error");
            break;
        default:
            swprintf_s(line, L"Unknown hook state: %ld", hookState);
            break;
        }
        AppendStatusLine(line);
    }

    const LONG currentSerial = InterlockedCompareExchange(&g_app.shared->eventSerial, 0, 0);
    LONG first = g_app.lastEventSerial + 1;
    if (currentSerial - first + 1 > static_cast<LONG>(ReflexProbeProtocol::kEventCapacity)) {
        first = currentSerial - static_cast<LONG>(ReflexProbeProtocol::kEventCapacity) + 1;
        g_app.lastEventSerial = first - 1;
    }

    LONG processedSerial = g_app.lastEventSerial;
    for (LONG serial = first; serial <= currentSerial; ++serial) {
        if (serial <= 0)
            continue;

        const uint32_t index = static_cast<uint32_t>(serial - 1) % ReflexProbeProtocol::kEventCapacity;
        const ReflexProbeProtocol::ReflexEvent& event = g_app.shared->events[index];
        const LONG storedSerial = InterlockedCompareExchange(
            const_cast<volatile LONG*>(&event.sequence), 0, 0);
        if (storedSerial != serial)
            break;

        const wchar_t* mode = event.mode == 2 ? L"On + Boost" :
                              (event.mode == 1 ? L"On" : L"Off");
        const wchar_t* callName = BackendCallName(backend);
        wchar_t line[384]{};
        if (event.requestedUs)
            swprintf_s(line,
                L"%s #%ld: mode=%s, requested=%u us (%.3f FPS), effective=%u us (%.3f FPS), result=%ld",
                callName, serial, mode,
                event.requestedUs, 1000000.0 / static_cast<double>(event.requestedUs),
                event.effectiveUs, event.effectiveUs ? 1000000.0 / static_cast<double>(event.effectiveUs) : 0.0,
                event.result);
        else if (event.effectiveUs)
            swprintf_s(line,
                L"%s #%ld: mode=%s, requested=0 us (automatic), effective=%u us (%.3f FPS), result=%ld",
                callName, serial, mode,
                event.effectiveUs, 1000000.0 / static_cast<double>(event.effectiveUs), event.result);
        else
            swprintf_s(line,
                L"%s #%ld: mode=%s, requested=0 us (automatic), effective=0 us (pass-through), result=%ld",
                callName, serial, mode, event.result);

        AppendStatusLineAtQpc(line, event.qpc);
        processedSerial = serial;
    }

    if (processedSerial > g_app.lastEventSerial)
        g_app.lastEventSerial = processedSerial;
}

void BrowseForGame()
{
    wchar_t path[ReflexProbeProtocol::kPathChars]{};
    GetWindowTextW(g_app.gamePath, path, static_cast<int>(_countof(path)));

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app.window;
    dialog.lpstrFilter = L"Windows executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(_countof(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&dialog))
        SetWindowTextW(g_app.gamePath, path);
}

void LayoutControls(int clientWidth, int clientHeight)
{
    if (clientWidth <= 0 || clientHeight <= 0)
        return;

    const int usableWidth = clientWidth - (kMargin * 2);
    const int browseWidth = 96;
    const int launchWidth = 144;
    const int wrapWidth = 104;

    if (g_app.gameLabel)
        MoveWindow(g_app.gameLabel, kMargin, 12, 150, 20, TRUE);
    if (g_app.browse)
        MoveWindow(g_app.browse, clientWidth - kMargin - browseWidth, 33, browseWidth, 26, TRUE);
    if (g_app.gamePath) {
        const int gameWidth = usableWidth - browseWidth - 10;
        MoveWindow(g_app.gamePath, kMargin, 34, gameWidth > 50 ? gameWidth : 50, 24, TRUE);
    }

    if (g_app.argumentsLabel)
        MoveWindow(g_app.argumentsLabel, kMargin, 68, 150, 20, TRUE);
    if (g_app.arguments)
        MoveWindow(g_app.arguments, kMargin, 90, usableWidth > 50 ? usableWidth : 50, 24, TRUE);

    if (g_app.overrideEnable)
        MoveWindow(g_app.overrideEnable, kMargin, 128, 190, 22, TRUE);
    if (g_app.overrideFps)
        MoveWindow(g_app.overrideFps, 210, 126, 80, 24, TRUE);
    if (g_app.fpsLabel)
        MoveWindow(g_app.fpsLabel, 298, 130, 40, 20, TRUE);
    if (g_app.applyOverride)
        MoveWindow(g_app.applyOverride, 350, 124, 120, 28, TRUE);
    if (g_app.launch)
        MoveWindow(g_app.launch, clientWidth - kMargin - launchWidth, 124, launchWidth, 30, TRUE);

    if (g_app.statusLabel)
        MoveWindow(g_app.statusLabel, kMargin, 164, 180, 20, TRUE);
    if (g_app.wordWrap)
        MoveWindow(g_app.wordWrap, clientWidth - kMargin - wrapWidth, 162, wrapWidth, 22, TRUE);

    if (g_app.status) {
        int statusHeight = clientHeight - kStatusTop - kMargin;
        if (statusHeight < 80)
            statusHeight = 80;
        MoveWindow(g_app.status, kMargin, kStatusTop, usableWidth > 50 ? usableWidth : 50, statusHeight, TRUE);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_app.window = window;

        g_app.gameLabel = CreateWindowExW(0, L"STATIC", L"Game executable",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.gameLabel, font);

        g_app.gamePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_GAME_PATH), g_app.instance, nullptr);
        SetChildFont(g_app.gamePath, font);

        g_app.browse = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_BROWSE), g_app.instance, nullptr);
        SetChildFont(g_app.browse, font);

        g_app.argumentsLabel = CreateWindowExW(0, L"STATIC", L"Arguments (optional)",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.argumentsLabel, font);

        g_app.arguments = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_ARGUMENTS), g_app.instance, nullptr);
        SetChildFont(g_app.arguments, font);

        g_app.overrideEnable = CreateWindowExW(0, L"BUTTON", L"Override Reflex frame limit",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_OVERRIDE_ENABLE), g_app.instance, nullptr);
        SetChildFont(g_app.overrideEnable, font);

        g_app.overrideFps = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"165",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_OVERRIDE_FPS), g_app.instance, nullptr);
        SetChildFont(g_app.overrideFps, font);

        g_app.fpsLabel = CreateWindowExW(0, L"STATIC", L"FPS",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.fpsLabel, font);

        g_app.applyOverride = CreateWindowExW(0, L"BUTTON", L"Apply Override",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_APPLY_OVERRIDE), g_app.instance, nullptr);
        SetChildFont(g_app.applyOverride, font);

        g_app.launch = CreateWindowExW(0, L"BUTTON", L"Launch + Inject",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LAUNCH), g_app.instance, nullptr);
        SetChildFont(g_app.launch, font);

        g_app.statusLabel = CreateWindowExW(0, L"STATIC", L"Status / Reflex requests",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.statusLabel, font);

        g_app.wordWrap = CreateWindowExW(0, L"BUTTON", L"Word wrap",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_WORD_WRAP), g_app.instance, nullptr);
        SetChildFont(g_app.wordWrap, font);
        Button_SetCheck(g_app.wordWrap, BST_CHECKED);

        RecreateStatusControl(true);

        RECT client{};
        GetClientRect(window, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);

        AppendStatusLine(L"ReflexProbe bootstrap: direct-launch Streamline observer/override.");
        AppendStatusLine(L"Override is OFF by default. Anti-cheat/protected games are intentionally out of scope.");
        LogBinaryIdentity();
        SetTimer(window, kPollTimer, 100, nullptr);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = kMinimumWindowWidth;
        info->ptMinTrackSize.y = kMinimumWindowHeight;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BROWSE:
            BrowseForGame();
            return 0;
        case IDC_APPLY_OVERRIDE:
            ApplyOverrideToShared();
            return 0;
        case IDC_LAUNCH:
            LaunchAndInject();
            return 0;
        case IDC_WORD_WRAP:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool wordWrap = Button_GetCheck(g_app.wordWrap) == BST_CHECKED;
                RecreateStatusControl(wordWrap);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == kPollTimer) {
            PollSharedState();
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, kPollTimer);
        CleanupTarget();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void CenterWindowOnChosenMonitor(HWND window)
{
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect))
        return;

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;

    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;

    int x = monitorInfo.rcWork.left + (workWidth - width) / 2;
    int y = monitorInfo.rcWork.top + (workHeight - height) / 2;
    if (x < monitorInfo.rcWork.left)
        x = monitorInfo.rcWork.left;
    if (y < monitorInfo.rcWork.top)
        y = monitorInfo.rcWork.top;

    SetWindowPos(window, nullptr, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_app.instance = instance;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    HWND window = CreateWindowExW(0, kWindowClass, L"ReflexProbe",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 650,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;

    CenterWindowOnChosenMonitor(window);
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

#undef RP_WIDEN
#undef RP_WIDEN_INNER
