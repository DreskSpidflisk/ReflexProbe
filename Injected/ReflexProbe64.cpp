#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <winver.h>

#include <sl.h>
#include <sl_reflex.h>

#include "../Common/Protocol.h"

#pragma comment(lib, "version.lib")

namespace {

HANDLE g_mapping = nullptr;
ReflexProbeProtocol::SharedState* g_shared = nullptr;
PFun_slGetFeatureFunction* g_realGetFeatureFunction = nullptr;
PFun_slReflexSetOptions* g_realReflexSetOptions = nullptr;

void BuildMappingName(DWORD processId, wchar_t* out, size_t outCount)
{
    swprintf_s(out, outCount, L"%s%lu", ReflexProbeProtocol::kMappingPrefix, processId);
}

void SetSharedError(const wchar_t* text)
{
    if (g_shared) {
        wcsncpy_s(g_shared->lastError, _countof(g_shared->lastError), text ? text : L"", _TRUNCATE);
        MemoryBarrier();
        InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateError);
    }

    if (text) {
        OutputDebugStringW(L"ReflexProbe64: ");
        OutputDebugStringW(text);
        OutputDebugStringW(L"\n");
    }
}

bool ConnectSharedState()
{
    wchar_t mappingName[128]{};
    BuildMappingName(GetCurrentProcessId(), mappingName, _countof(mappingName));

    for (int attempt = 0; attempt < 1000; ++attempt) {
        g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
        if (g_mapping)
            break;
        Sleep(10);
    }

    if (!g_mapping) {
        OutputDebugStringW(L"ReflexProbe64: controller shared memory was not found.\n");
        return false;
    }

    g_shared = reinterpret_cast<ReflexProbeProtocol::SharedState*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ReflexProbeProtocol::SharedState)));
    if (!g_shared) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        OutputDebugStringW(L"ReflexProbe64: MapViewOfFile failed.\n");
        return false;
    }

    if (g_shared->magic != ReflexProbeProtocol::kMagic ||
        g_shared->version != ReflexProbeProtocol::kVersion ||
        g_shared->structSize != sizeof(ReflexProbeProtocol::SharedState)) {
        OutputDebugStringW(L"ReflexProbe64: shared-memory protocol mismatch.\n");
        UnmapViewOfFile(g_shared);
        CloseHandle(g_mapping);
        g_shared = nullptr;
        g_mapping = nullptr;
        return false;
    }

    return true;
}

void GetFileVersionString(const wchar_t* path, wchar_t* out, size_t outCount)
{
    if (!out || !outCount)
        return;
    out[0] = 0;

    DWORD ignored = 0;
    DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    if (!bytes)
        return;

    void* data = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!data)
        return;

    if (GetFileVersionInfoW(path, 0, bytes, data)) {
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT fixedBytes = 0;
        if (VerQueryValueW(data, L"\\", reinterpret_cast<void**>(&fixed), &fixedBytes) &&
            fixed && fixedBytes >= sizeof(VS_FIXEDFILEINFO)) {
            swprintf_s(out, outCount, L"%u.%u.%u.%u",
                HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
        }
    }

    HeapFree(GetProcessHeap(), 0, data);
}

void PublishEvent(LONG mode, uint32_t requestedUs, uint32_t effectiveUs, LONG result)
{
    if (!g_shared)
        return;

    const LONG serial = InterlockedIncrement(&g_shared->eventSerial);
    const uint32_t index = static_cast<uint32_t>(serial - 1) % ReflexProbeProtocol::kEventCapacity;
    ReflexProbeProtocol::ReflexEvent& event = g_shared->events[index];

    InterlockedExchange(&event.sequence, 0);

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.mode = mode;
    event.requestedUs = requestedUs;
    event.effectiveUs = effectiveUs;
    event.result = result;

    MemoryBarrier();
    InterlockedExchange(&event.sequence, serial);
}

sl::Result HookReflexSetOptions(const sl::ReflexOptions& options)
{
    PFun_slReflexSetOptions* real = g_realReflexSetOptions;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    sl::ReflexOptions forwarded = options;
    uint32_t effectiveUs = options.frameLimitUs;

    if (g_shared && InterlockedCompareExchange(&g_shared->overrideEnabled, 0, 0) != 0) {
        const LONG configured = InterlockedCompareExchange(&g_shared->overrideUs, 0, 0);
        if (configured > 0) {
            effectiveUs = static_cast<uint32_t>(configured);
            forwarded.frameLimitUs = effectiveUs;
        }
    }

    const sl::Result result = real(forwarded);
    PublishEvent(static_cast<LONG>(options.mode), options.frameLimitUs, effectiveUs, static_cast<LONG>(result));
    return result;
}

void PublishReflexFunction(void* function)
{
    if (!g_shared || !function)
        return;

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(function, &memory, sizeof(memory))) {
        HMODULE module = reinterpret_cast<HMODULE>(memory.AllocationBase);
        DWORD count = GetModuleFileNameW(module, g_shared->reflexPath,
            static_cast<DWORD>(_countof(g_shared->reflexPath)));
        if (!count || count >= _countof(g_shared->reflexPath))
            wcscpy_s(g_shared->reflexPath, L"sl.reflex.dll");
    } else {
        wcscpy_s(g_shared->reflexPath, L"sl.reflex.dll");
    }

    GetFileVersionString(g_shared->reflexPath, g_shared->reflexVersion,
        _countof(g_shared->reflexVersion));

    MemoryBarrier();
    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateHooked);
}

sl::Result HookGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
    PFun_slGetFeatureFunction* real = g_realGetFeatureFunction;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    const sl::Result result = real(feature, functionName, function);
    if (result != sl::Result::eOk || !functionName || !function)
        return result;

    if (strcmp(functionName, "slReflexSetOptions") == 0) {
        g_realReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(function);
        PublishReflexFunction(function);
        function = reinterpret_cast<void*>(&HookReflexSetOptions);
    }

    return result;
}

bool PatchResolverImportInModule(HMODULE module, bool& sawTarget)
{
    if (!module)
        return false;

    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, "sl.interposer.dll") != 0 || !descriptor->FirstThunk)
            continue;

        HMODULE interposer = GetModuleHandleA(dllName);
        if (!interposer)
            continue;

        void* real = reinterpret_cast<void*>(GetProcAddress(interposer, "slGetFeatureFunction"));
        if (!real)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            void* current = reinterpret_cast<void*>(static_cast<uintptr_t>(thunk->u1.Function));
            if (current == reinterpret_cast<void*>(&HookGetFeatureFunction)) {
                sawTarget = true;
                return true;
            }
            if (current != real)
                continue;

            sawTarget = true;
            if (!g_realGetFeatureFunction)
                g_realGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(real);

            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &oldProtect)) {
                wchar_t error[192]{};
                swprintf_s(error, L"VirtualProtect failed while patching the slGetFeatureFunction IAT slot (%lu).",
                    GetLastError());
                SetSharedError(error);
                return false;
            }

            InterlockedExchangePointer(
                reinterpret_cast<PVOID volatile*>(&thunk->u1.Function),
                reinterpret_cast<void*>(&HookGetFeatureFunction));

            DWORD ignored = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &ignored)) {
                wchar_t error[192]{};
                swprintf_s(error, L"VirtualProtect failed restoring the slGetFeatureFunction IAT slot (%lu).",
                    GetLastError());
                SetSharedError(error);
                return false;
            }

            return true;
        }
    }

    return false;
}

bool PatchLoadedResolverImports(bool& foundAny)
{
    foundAny = false;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool success = true;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            bool sawTarget = false;
            PatchResolverImportInModule(entry.hModule, sawTarget);
            foundAny = foundAny || sawTarget;

            if (g_shared && InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
                    ReflexProbeProtocol::HookStateError) {
                success = false;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return success;
}

DWORD WINAPI WorkerThread(void*)
{
    if (!ConnectSharedState())
        return 1;

    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateWaitingForDll);

    bool announcedResolver = false;

    // Direct-launch bootstrap: patch imports before the application asks Streamline for
    // feature function pointers. This avoids modifying NVIDIA executable code entirely.
    for (uint32_t attempt = 0;; ++attempt) {
        bool foundResolverImport = false;
        if (!PatchLoadedResolverImports(foundResolverImport))
            return 1;

        if (foundResolverImport && !announcedResolver) {
            OutputDebugStringW(L"ReflexProbe64: intercepted imported slGetFeatureFunction resolver.\n");
            announcedResolver = true;
        }

        if (InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
                ReflexProbeProtocol::HookStateHooked)
            return 0;

        // Loaded engine DLLs can add another Streamline import later in startup.
        Sleep(attempt < 10000 ? 1 : 50);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}
