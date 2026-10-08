#include "ControllerInternal.h"

#include <tlhelp32.h>
#include <bcrypt.h>
#include <wchar.h>
#include <stdio.h>

#pragma comment(lib, "bcrypt.lib")

#define RP_WIDEN_INNER(x) L##x
#define RP_WIDEN(x) RP_WIDEN_INNER(x)

namespace ReflexProbeController {

constexpr wchar_t kControllerBuildStamp[] = RP_WIDEN(__DATE__) L" " RP_WIDEN(__TIME__);
constexpr DWORD kLaunchVerificationTimeoutMs = 1000;
constexpr DWORD kLaunchVerificationPollMs = 5;

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

bool InjectDll(HANDLE process, DWORD processId, const wchar_t* dllPath,
               bool allowPostLoadVerificationRecovery,
               wchar_t* error, size_t errorCount)
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

    DWORD remoteExitCode = 0;
    bool haveRemoteExitCode = false;
    DWORD remoteExitCodeError = ERROR_SUCCESS;
    if (wait == WAIT_OBJECT_0) {
        if (GetExitCodeThread(thread, &remoteExitCode)) {
            haveRemoteExitCode = true;
        } else {
            remoteExitCodeError = GetLastError();
        }
    }

    CloseHandle(thread);

    if (wait != WAIT_OBJECT_0) {
        swprintf_s(error, errorCount, L"Remote LoadLibraryW did not finish in 10 seconds.");
        return false;
    }

    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    // Preserve the original successful path exactly: one immediate Toolhelp module check.
    if (FindRemoteModuleBase(processId, L"ReflexProbe64.dll"))
        return true;

    // The false negative has only been observed when Launch + Inject loads into a process
    // that ReflexProbe itself just created. Watch/Attach deliberately keep the old one-shot
    // behavior until there is evidence that those acquisition paths need this fallback.
    if (!allowPostLoadVerificationRecovery) {
        swprintf_s(error, errorCount,
            L"ReflexProbe64.dll was not present after LoadLibraryW returned.");
        return false;
    }

    bool recoveredByHandshake = false;
    bool recoveredByModule = false;
    const ULONGLONG verificationEnd =
        GetTickCount64() + static_cast<ULONGLONG>(kLaunchVerificationTimeoutMs);

    do {
        if (g_app.shared && g_app.shared->injectedBuild[0]) {
            recoveredByHandshake = true;
            break;
        }

        if (FindRemoteModuleBase(processId, L"ReflexProbe64.dll")) {
            recoveredByModule = true;
            break;
        }

        Sleep(kLaunchVerificationPollMs);
    } while (GetTickCount64() < verificationEnd);

    if (recoveredByHandshake || recoveredByModule) {
        AppendStatusLine(recoveredByHandshake
            ? L"Launch injection verification recovered after the initial module snapshot miss: injected DLL shared-memory handshake observed."
            : L"Launch injection verification recovered after the initial module snapshot miss: ReflexProbe64.dll became visible on retry.");
        return true;
    }

    if (haveRemoteExitCode) {
        swprintf_s(error, errorCount,
            L"ReflexProbe64.dll could not be verified within %lu ms after LoadLibraryW returned. "
            L"Remote thread exit code: 0x%08lX. No module snapshot or injected build handshake was observed.",
            kLaunchVerificationTimeoutMs, remoteExitCode);
    } else {
        swprintf_s(error, errorCount,
            L"ReflexProbe64.dll could not be verified within %lu ms after LoadLibraryW returned. "
            L"Remote thread exit code was unavailable (GetExitCodeThread error %lu). "
            L"No module snapshot or injected build handshake was observed.",
            kLaunchVerificationTimeoutMs, remoteExitCodeError);
    }

    return false;
}

bool CreateSharedState(DWORD processId, const wchar_t* targetPath,
                       bool overrideEnabled, uint32_t overrideUs, bool forceBoostWhenOn,
                       bool countReflexSleepCalls, bool probeReflex, bool probeDlssFg,
                       const FgPolicy& fgPolicy, wchar_t* error, size_t errorCount)
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
    g_app.shared->forceBoostWhenOn = forceBoostWhenOn ? 1 : 0;
    g_app.shared->countReflexSleepCalls = countReflexSleepCalls && probeReflex ? 1 : 0;
    g_app.shared->probeReflexEnabled = probeReflex ? 1 : 0;
    g_app.shared->probeDlssFgEnabled = probeDlssFg ? 1 : 0;
    g_app.shared->wrapFgGetState = probeDlssFg && fgPolicy.wrapGetState ? 1 : 0;
    g_app.shared->fgForceOnWhenAuto = probeDlssFg && fgPolicy.forceOnWhenAuto ? 1 : 0;
    g_app.shared->fgMenuOverrideEnabled = probeDlssFg && fgPolicy.overrideMenu ? 1 : 0;
    g_app.shared->fgMenuOverrideOn = fgPolicy.menuDetectionOn ? 1 : 0;
    g_app.shared->fgRetentionOverrideEnabled =
        probeDlssFg && fgPolicy.overrideRetention ? 1 : 0;
    g_app.shared->fgRetentionOverrideOn = fgPolicy.retainResourcesWhenOff ? 1 : 0;
    wcsncpy_s(g_app.shared->targetPath, _countof(g_app.shared->targetPath), targetPath, _TRUNCATE);
    return true;
}


struct ProcessMatch {
    DWORD processId = 0;
    DWORD parentProcessId = 0;
    wchar_t imagePath[ReflexProbeProtocol::kPathChars]{};
};

double QpcMilliseconds(LONGLONG start, LONGLONG end)
{
    if (start <= 0 || end < start)
        return 0.0;

    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return 0.0;

    return (static_cast<double>(end - start) * 1000.0) /
        static_cast<double>(frequency.QuadPart);
}

bool NormalizeTargetPath(const wchar_t* path, wchar_t* out, size_t outCount)
{
    if (!path || !*path || !out || !outCount)
        return false;

    const DWORD chars = GetFullPathNameW(path, static_cast<DWORD>(outCount), out, nullptr);
    return chars > 0 && chars < outCount;
}

bool QueryProcessImagePath(DWORD processId, wchar_t* out, size_t outCount)
{
    if (!out || !outCount)
        return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;

    DWORD chars = static_cast<DWORD>(outCount);
    const BOOL ok = QueryFullProcessImageNameW(process, 0, out, &chars);
    CloseHandle(process);

    if (!ok || !chars || chars >= outCount) {
        out[0] = 0;
        return false;
    }

    return true;
}

bool FindMatchingProcess(const wchar_t* targetPath, ProcessMatch& match)
{
    match = {};

    const wchar_t* targetName = PathFileName(targetPath);
    if (!targetName || !*targetName)
        return false;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, targetName) != 0)
                continue;

            wchar_t imagePath[ReflexProbeProtocol::kPathChars]{};
            if (!QueryProcessImagePath(entry.th32ProcessID, imagePath, _countof(imagePath)))
                continue;

            if (_wcsicmp(imagePath, targetPath) != 0)
                continue;

            match.processId = entry.th32ProcessID;
            match.parentProcessId = entry.th32ParentProcessID;
            wcscpy_s(match.imagePath, imagePath);
            found = true;
            break;
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

void LogInitialTargetPolicy(bool overrideEnabled, uint32_t overrideUs, bool forceBoostWhenOn,
                            bool countReflexSleepCalls, bool probeReflex, bool probeDlssFg,
                            const FgPolicy& fgPolicy)
{
    AppendStatusLine(probeReflex
        ? L"Reflex probing: enabled; supported Reflex function pointers may be wrapped."
        : L"Reflex probing: disabled; Reflex function pointers pass through untouched.");
    AppendStatusLine(probeDlssFg
        ? (fgPolicy.wrapGetState
            ? L"DLSS FG probing: SetOptions and GetState intercepted."
            : L"DLSS FG probing: SetOptions intercepted; GetState pointer untouched.")
        : L"DLSS FG probing: disabled; FG function pointers pass through untouched.");
    if (probeDlssFg) {
        AppendStatusLine(fgPolicy.forceOnWhenAuto
            ? L"Initial FG Auto-to-On override: enabled."
            : L"Initial FG Auto-to-On override: disabled.");
        AppendStatusLine(!fgPolicy.overrideMenu
            ? L"Initial FG menu-detection override: disabled."
            : (fgPolicy.menuDetectionOn
                ? L"Initial FG menu-detection override: On."
                : L"Initial FG menu-detection override: Off."));
        AppendStatusLine(!fgPolicy.overrideRetention
            ? L"Initial FG resource-retention override: disabled."
            : (fgPolicy.retainResourcesWhenOff
                ? L"Initial FG resource-retention override: On (VRAM retained while FG Off)."
                : L"Initial FG resource-retention override: Off."));
    }

    wchar_t line[256]{};
    if (probeReflex && overrideEnabled && overrideUs) {
        swprintf_s(line, L"Initial override: %u us (%.3f FPS).", overrideUs,
            1000000.0 / static_cast<double>(overrideUs));
        AppendStatusLine(line);
    } else if (probeReflex && overrideEnabled) {
        AppendStatusLine(L"Initial override: 0 us (no explicit Reflex frame limit).");
    } else if (probeReflex) {
        AppendStatusLine(L"Initial frame-limit override: disabled.");
    }

    if (probeReflex && forceBoostWhenOn)
        AppendStatusLine(L"Initial Force Boost policy: enabled; plain Reflex On requests will be forwarded as On + Boost.");
    else if (probeReflex)
        AppendStatusLine(L"Initial Force Boost policy: disabled; Reflex mode requests pass through unchanged.");

    if (probeReflex)
        AppendStatusLine(countReflexSleepCalls
            ? L"Initial Reflex Sleep call counting: enabled; Reflex Sleep will be wrapped for this target."
            : L"Initial Reflex Sleep call counting: disabled; Reflex Sleep will remain untouched for this target.");

    if (g_app.captureMode == CaptureModeRawDebug)
        AppendStatusLine(L"Capture mode: Raw debug; bounded raw retention starts with this target.");
    else
        AppendStatusLine(L"Capture mode: State changes; identical Reflex and per-viewport FG calls are counted.");
}

bool PrepareCaptureForExternalTarget(wchar_t* error, size_t errorCount)
{
    if (g_app.captureMode == CaptureModeRawDebug) {
        if (!EnsureRawDebugBuffer()) {
            swprintf_s(error, errorCount, L"Could not allocate the bounded Raw debug capture buffer.");
            return false;
        }
        ResetRawDebugCapture();
    }

    ResetLiveStateTracking();
    return true;
}

bool InjectMatchedProcess(const ProcessMatch& match,
                          bool overrideEnabled, uint32_t overrideUs, bool forceBoostWhenOn,
                          bool countReflexSleepCalls, bool probeReflex, bool probeDlssFg,
                          const FgPolicy& fgPolicy, LONGLONG detectedQpc, LONGLONG watchStartQpc,
                          const wchar_t* acquisitionName,
                          wchar_t* error, size_t errorCount)
{
    wchar_t line[2300]{};

    if (watchStartQpc > 0) {
        swprintf_s(line,
            L"Process detected:\r\nPID: %lu\r\nParent PID: %lu\r\nImage: %s\r\nWatch elapsed: %.3f ms",
            match.processId, match.parentProcessId, match.imagePath,
            QpcMilliseconds(watchStartQpc, detectedQpc));
    } else {
        swprintf_s(line,
            L"Process detected:\r\nPID: %lu\r\nParent PID: %lu\r\nImage: %s\r\nAcquisition: %s",
            match.processId, match.parentProcessId, match.imagePath,
            acquisitionName ? acquisitionName : L"Attach");
    }
    AppendStatusLineAtQpc(line, detectedQpc);

    const DWORD access =
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE |
        PROCESS_VM_READ |
        SYNCHRONIZE;

    HANDLE target = OpenProcess(access, FALSE, match.processId);
    LARGE_INTEGER openedQpc{};
    QueryPerformanceCounter(&openedQpc);

    if (!target) {
        swprintf_s(error, errorCount, L"OpenProcess failed for PID %lu (%lu).",
            match.processId, GetLastError());
        return false;
    }

    swprintf_s(line,
        L"Process opened:\r\nPID: %lu\r\nImage: %s\r\n+%.3f ms after detection",
        match.processId, match.imagePath,
        QpcMilliseconds(detectedQpc, openedQpc.QuadPart));
    AppendStatusLineAtQpc(line, openedQpc.QuadPart);

    BOOL targetIsWow64 = FALSE;
    if (IsWow64Process(target, &targetIsWow64) && targetIsWow64) {
        CloseHandle(target);
        swprintf_s(error, errorCount, L"ReflexProbe supports x64 targets only.");
        return false;
    }

    if (FindRemoteModuleBase(match.processId, L"ReflexProbe64.dll")) {
        CloseHandle(target);
        swprintf_s(error, errorCount,
            L"ReflexProbe64.dll is already loaded in PID %lu. Refusing to inject a second controller instance.",
            match.processId);
        return false;
    }

    if (!PrepareCaptureForExternalTarget(error, errorCount)) {
        CloseHandle(target);
        return false;
    }

    g_app.process = target;
    g_app.processId = match.processId;

    if (!CreateSharedState(match.processId, match.imagePath,
            overrideEnabled, overrideUs, forceBoostWhenOn, countReflexSleepCalls,
            probeReflex, probeDlssFg, fgPolicy, error, errorCount)) {
        CleanupTarget();
        return false;
    }

    wchar_t dllPath[MAX_PATH]{};
    if (!GetSiblingDllPath(dllPath, _countof(dllPath)) ||
        GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        swprintf_s(error, errorCount,
            L"ReflexProbe64.dll was not found beside ReflexProbe.exe. Build both projects in the solution.");
        CleanupTarget();
        return false;
    }

    LARGE_INTEGER injectStartQpc{};
    QueryPerformanceCounter(&injectStartQpc);

    if (!InjectDll(target, match.processId, dllPath, false, error, errorCount)) {
        CleanupTarget();
        return false;
    }

    LARGE_INTEGER injectedQpc{};
    QueryPerformanceCounter(&injectedQpc);

    g_app.lastHookState = -1;
    g_app.lastFgHookBits = 0;
    g_app.lastEventSerial = 0;
    g_app.loggedInjectedBuild = false;
    g_app.rawUiBatchWarningShown = false;
    UpdateAcquisitionControls();

    swprintf_s(line,
        L"DLL injection complete:\r\nPID: %lu\r\nImage: %s\r\nInjection call: %.3f ms\r\n+%.3f ms after process open",
        match.processId, match.imagePath,
        QpcMilliseconds(injectStartQpc.QuadPart, injectedQpc.QuadPart),
        QpcMilliseconds(openedQpc.QuadPart, injectedQpc.QuadPart));
    AppendStatusLineAtQpc(line, injectedQpc.QuadPart);

    LogInitialTargetPolicy(overrideEnabled, overrideUs, forceBoostWhenOn,
        countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy);
    return true;
}

bool ArmProcessWatch(const wchar_t* targetPath, bool overrideEnabled, uint32_t overrideUs,
                     bool forceBoostWhenOn, bool countReflexSleepCalls,
                     bool probeReflex, bool probeDlssFg, const FgPolicy& fgPolicy,
                     wchar_t* error, size_t errorCount)
{
    if (g_app.process) {
        swprintf_s(error, errorCount, L"A target is already active.");
        return false;
    }
    if (g_app.watchArmed) {
        swprintf_s(error, errorCount, L"Process watch is already armed.");
        return false;
    }

    wchar_t normalized[ReflexProbeProtocol::kPathChars]{};
    if (!NormalizeTargetPath(targetPath, normalized, _countof(normalized))) {
        swprintf_s(error, errorCount, L"Could not normalize the selected game executable path.");
        return false;
    }

    const DWORD attributes = GetFileAttributesW(normalized);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        swprintf_s(error, errorCount, L"The selected game executable does not exist.");
        return false;
    }

    wchar_t dllPath[MAX_PATH]{};
    if (!GetSiblingDllPath(dllPath, _countof(dllPath)) ||
        GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        swprintf_s(error, errorCount,
            L"ReflexProbe64.dll was not found beside ReflexProbe.exe. Build both projects in the solution.");
        return false;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    wcscpy_s(g_app.watchTargetPath, normalized);
    g_app.watchOverrideEnabled = overrideEnabled;
    g_app.watchOverrideUs = overrideUs;
    g_app.watchForceBoostWhenOn = forceBoostWhenOn;
    g_app.watchCountReflexSleepCalls = countReflexSleepCalls;
    g_app.watchProbeReflex = probeReflex;
    g_app.watchProbeDlssFg = probeDlssFg;
    g_app.watchFgPolicy = fgPolicy;
    g_app.watchStartQpc = now.QuadPart;
    g_app.watchArmed = true;

    if (!SetTimer(g_app.window, kWatchTimer, kWatchPollIntervalMs, nullptr)) {
        g_app.watchArmed = false;
        g_app.watchTargetPath[0] = 0;
        g_app.watchStartQpc = 0;
        swprintf_s(error, errorCount, L"Could not start the process watch timer.");
        return false;
    }

    BeginNewDiagnosticSession();
    UpdateAcquisitionControls();

    wchar_t line[1400]{};
    swprintf_s(line,
        L"Watch armed:\r\nTarget: %s\r\nPolling: %u ms while armed; exact full-path match required.",
        g_app.watchTargetPath, static_cast<unsigned int>(kWatchPollIntervalMs));
    AppendStatusLineAtQpc(line, now.QuadPart);
    return true;
}

void CancelProcessWatch(bool logCancellation)
{
    if (!g_app.watchArmed)
        return;

    KillTimer(g_app.window, kWatchTimer);
    g_app.watchArmed = false;
    g_app.watchTargetPath[0] = 0;
    g_app.watchOverrideEnabled = false;
    g_app.watchOverrideUs = 0;
    g_app.watchForceBoostWhenOn = false;
    g_app.watchCountReflexSleepCalls = false;
    g_app.watchProbeReflex = true;
    g_app.watchProbeDlssFg = false;
    g_app.watchStartQpc = 0;
    UpdateAcquisitionControls();

    if (logCancellation)
        AppendStatusLine(L"Process watch cancelled.");
}

void PollProcessWatch()
{
    if (!g_app.watchArmed || g_app.process)
        return;

    ProcessMatch match{};
    if (!FindMatchingProcess(g_app.watchTargetPath, match))
        return;

    LARGE_INTEGER detectedQpc{};
    QueryPerformanceCounter(&detectedQpc);

    const bool overrideEnabled = g_app.watchOverrideEnabled;
    const uint32_t overrideUs = g_app.watchOverrideUs;
    const bool forceBoostWhenOn = g_app.watchForceBoostWhenOn;
    const bool countReflexSleepCalls = g_app.watchCountReflexSleepCalls;
    const bool probeReflex = g_app.watchProbeReflex;
    const bool probeDlssFg = g_app.watchProbeDlssFg;
    const FgPolicy fgPolicy = g_app.watchFgPolicy;
    const LONGLONG watchStartQpc = g_app.watchStartQpc;

    KillTimer(g_app.window, kWatchTimer);
    g_app.watchArmed = false;
    UpdateAcquisitionControls();

    wchar_t error[512]{};
    if (!InjectMatchedProcess(match, overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy,
            detectedQpc.QuadPart, watchStartQpc, L"Watch",
            error, _countof(error))) {
        wchar_t line[768]{};
        swprintf_s(line, L"Watch injection failed: %s", error);
        AppendStatusLineAtQpc(line, detectedQpc.QuadPart);
        MessageBoxW(g_app.window, error, L"ReflexProbe Watch + Inject", MB_ICONERROR);
        UpdateAcquisitionControls();
    }

    g_app.watchTargetPath[0] = 0;
    g_app.watchOverrideEnabled = false;
    g_app.watchOverrideUs = 0;
    g_app.watchForceBoostWhenOn = false;
    g_app.watchCountReflexSleepCalls = false;
    g_app.watchProbeReflex = true;
    g_app.watchProbeDlssFg = false;
    g_app.watchStartQpc = 0;
}

bool AttachRunningProcess(const wchar_t* targetPath, bool overrideEnabled, uint32_t overrideUs,
                          bool forceBoostWhenOn, bool countReflexSleepCalls,
                          bool probeReflex, bool probeDlssFg, const FgPolicy& fgPolicy,
                          wchar_t* error, size_t errorCount)
{
    if (g_app.process) {
        swprintf_s(error, errorCount, L"A target is already active.");
        return false;
    }
    if (g_app.watchArmed) {
        swprintf_s(error, errorCount, L"Cancel the active process watch before attaching.");
        return false;
    }

    wchar_t normalized[ReflexProbeProtocol::kPathChars]{};
    if (!NormalizeTargetPath(targetPath, normalized, _countof(normalized))) {
        swprintf_s(error, errorCount, L"Could not normalize the selected game executable path.");
        return false;
    }

    ProcessMatch match{};
    if (!FindMatchingProcess(normalized, match)) {
        swprintf_s(error, errorCount,
            L"No running process matches the selected executable path exactly.");
        return false;
    }

    LARGE_INTEGER detectedQpc{};
    QueryPerformanceCounter(&detectedQpc);

    BeginNewDiagnosticSession();
    return InjectMatchedProcess(match, overrideEnabled, overrideUs, forceBoostWhenOn,
        countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy,
        detectedQpc.QuadPart, 0, L"Attach", error, errorCount);
}

} // namespace ReflexProbeController

#undef RP_WIDEN
#undef RP_WIDEN_INNER
