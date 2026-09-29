#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <winver.h>

#include <MinHook.h>
#include <sl_reflex.h>

#include "../Common/Protocol.h"

#pragma comment(lib, "version.lib")

namespace {

using PFunSlGetPluginFunction = void* (*)(const char* functionName);

HANDLE g_mapping = nullptr;
ReflexProbeProtocol::SharedState* g_shared = nullptr;
PFun_slReflexSetOptions* g_realReflexSetOptions = nullptr;
void* g_hookTarget = nullptr;

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
    if (!g_realReflexSetOptions)
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

    const sl::Result result = g_realReflexSetOptions(forwarded);
    PublishEvent(static_cast<LONG>(options.mode), options.frameLimitUs, effectiveUs, static_cast<LONG>(result));
    return result;
}

bool InstallReflexHook(HMODULE reflexModule)
{
    if (!g_shared || !reflexModule)
        return false;

    DWORD count = GetModuleFileNameW(reflexModule, g_shared->reflexPath,
        static_cast<DWORD>(_countof(g_shared->reflexPath)));
    if (!count || count >= _countof(g_shared->reflexPath))
        wcscpy_s(g_shared->reflexPath, L"sl.reflex.dll");

    GetFileVersionString(g_shared->reflexPath, g_shared->reflexVersion,
        _countof(g_shared->reflexVersion));
    MemoryBarrier();
    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateReflexFound);

    auto getPluginFunction = reinterpret_cast<PFunSlGetPluginFunction>(
        GetProcAddress(reflexModule, "slGetPluginFunction"));
    if (!getPluginFunction) {
        SetSharedError(L"sl.reflex.dll does not export slGetPluginFunction.");
        return false;
    }

    g_hookTarget = getPluginFunction("slReflexSetOptions");
    if (!g_hookTarget) {
        SetSharedError(L"slGetPluginFunction did not return slReflexSetOptions.");
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        wchar_t error[160]{};
        swprintf_s(error, L"MH_Initialize failed (%d).", static_cast<int>(status));
        SetSharedError(error);
        return false;
    }

    status = MH_CreateHook(g_hookTarget,
        reinterpret_cast<LPVOID>(&HookReflexSetOptions),
        reinterpret_cast<LPVOID*>(&g_realReflexSetOptions));
    if (status != MH_OK) {
        wchar_t error[160]{};
        swprintf_s(error, L"MH_CreateHook(slReflexSetOptions) failed (%d).", static_cast<int>(status));
        SetSharedError(error);
        return false;
    }

    status = MH_EnableHook(g_hookTarget);
    if (status != MH_OK) {
        wchar_t error[160]{};
        swprintf_s(error, L"MH_EnableHook(slReflexSetOptions) failed (%d).", static_cast<int>(status));
        SetSharedError(error);
        return false;
    }

    MemoryBarrier();
    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateHooked);
    return true;
}

DWORD WINAPI WorkerThread(void*)
{
    if (!ConnectSharedState())
        return 1;

    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateWaitingForDll);

    // Polling is deliberate for the bootstrap build. It keeps us out of loader-lock callbacks,
    // and Streamline loads sl.reflex.dll well before normal gameplay begins.
    for (uint32_t attempt = 0;; ++attempt) {
        HMODULE reflexModule = GetModuleHandleW(L"sl.reflex.dll");
        if (reflexModule) {
            InstallReflexHook(reflexModule);
            return 0;
        }

        // Be aggressive during startup, then become practically idle if this is not a Streamline title.
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
