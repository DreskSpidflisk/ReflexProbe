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

} // namespace ReflexProbeController

#undef RP_WIDEN
#undef RP_WIDEN_INNER
