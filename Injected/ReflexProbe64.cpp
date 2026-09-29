#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <winver.h>

#include <sl.h>
#include <sl_reflex.h>

#include "../Common/Protocol.h"

#pragma comment(lib, "version.lib")

namespace {

// Streamline 1.x configured Reflex through the generic slSetFeatureConstants API.
// Keep the old ABI local instead of dragging an obsolete Streamline SDK into the build.
constexpr uint32_t kLegacyFeatureReflex = 3;

struct LegacyReflexConstants {
    int32_t mode;
    uint32_t frameLimitUs;
    bool useMarkersToOptimize;
    uint16_t virtualKey;
    void* ext;
};

static_assert(offsetof(LegacyReflexConstants, mode) == 0);
static_assert(offsetof(LegacyReflexConstants, frameLimitUs) == 4);
static_assert(offsetof(LegacyReflexConstants, useMarkersToOptimize) == 8);
static_assert(offsetof(LegacyReflexConstants, virtualKey) == 10);
static_assert(offsetof(LegacyReflexConstants, ext) == 16);
static_assert(sizeof(LegacyReflexConstants) == 24);

using PFunLegacySetFeatureConstants = bool(uint32_t feature, const void* constants,
                                           uint32_t frameIndex, uint32_t id);

HANDLE g_mapping = nullptr;
ReflexProbeProtocol::SharedState* g_shared = nullptr;
PFun_slGetFeatureFunction* g_realGetFeatureFunction = nullptr;
PFun_slReflexSetOptions* g_realReflexSetOptions = nullptr;
PFunLegacySetFeatureConstants* g_realLegacySetFeatureConstants = nullptr;

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

uint32_t ApplyConfiguredOverride(uint32_t requestedUs)
{
    if (!g_shared || InterlockedCompareExchange(&g_shared->overrideEnabled, 0, 0) == 0)
        return requestedUs;

    const LONG configured = InterlockedCompareExchange(&g_shared->overrideUs, 0, 0);
    return configured > 0 ? static_cast<uint32_t>(configured) : requestedUs;
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
            wcscpy_s(g_shared->reflexPath, L"Streamline module");
    } else {
        wcscpy_s(g_shared->reflexPath, L"Streamline module");
    }

    GetFileVersionString(g_shared->reflexPath, g_shared->reflexVersion,
        _countof(g_shared->reflexVersion));

    MemoryBarrier();
    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateHooked);
}

sl::Result HookReflexSetOptions(const sl::ReflexOptions& options)
{
    PFun_slReflexSetOptions* real = g_realReflexSetOptions;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    sl::ReflexOptions forwarded = options;
    const uint32_t effectiveUs = ApplyConfiguredOverride(options.frameLimitUs);
    forwarded.frameLimitUs = effectiveUs;

    const sl::Result result = real(forwarded);
    PublishEvent(static_cast<LONG>(options.mode), options.frameLimitUs, effectiveUs, static_cast<LONG>(result));
    return result;
}

bool HookLegacySetFeatureConstants(uint32_t feature, const void* constants,
                                   uint32_t frameIndex, uint32_t id)
{
    PFunLegacySetFeatureConstants* real = g_realLegacySetFeatureConstants;
    if (!real)
        return false;

    if (feature != kLegacyFeatureReflex || !constants)
        return real(feature, constants, frameIndex, id);

    const auto* requested = static_cast<const LegacyReflexConstants*>(constants);
    LegacyReflexConstants forwarded = *requested;
    const uint32_t effectiveUs = ApplyConfiguredOverride(requested->frameLimitUs);
    forwarded.frameLimitUs = effectiveUs;

    const bool result = real(feature, &forwarded, frameIndex, id);
    // Modern Streamline uses result 0 for success. Normalize the old bool API so the
    // controller can keep one event format across both generations.
    PublishEvent(static_cast<LONG>(requested->mode), requested->frameLimitUs,
        effectiveUs, result ? 0 : 1);
    return result;
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

bool PatchImportSlot(IMAGE_THUNK_DATA64* thunk, void* replacement, const wchar_t* functionName)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &oldProtect)) {
        wchar_t error[224]{};
        swprintf_s(error, L"VirtualProtect failed while patching the %s IAT slot (%lu).",
            functionName, GetLastError());
        SetSharedError(error);
        return false;
    }

    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&thunk->u1.Function), replacement);

    DWORD ignored = 0;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &ignored)) {
        wchar_t error[224]{};
        swprintf_s(error, L"VirtualProtect failed restoring the %s IAT slot (%lu).",
            functionName, GetLastError());
        SetSharedError(error);
        return false;
    }

    return true;
}

bool PatchStreamlineImportsInModule(HMODULE module, bool& sawTarget)
{
    sawTarget = false;
    if (!module)
        return false;

    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return true;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return true;

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return true;

    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    if (!interposer)
        return true;

    void* modernResolver = reinterpret_cast<void*>(GetProcAddress(interposer, "slGetFeatureFunction"));
    void* legacySetConstants = reinterpret_cast<void*>(GetProcAddress(interposer, "slSetFeatureConstants"));

    // Streamline 2.x+ has the feature-function resolver. Streamline 1.x does not;
    // in that generation ReflexOptions were ReflexConstants sent through the generic setter.
    const bool useLegacyPath = modernResolver == nullptr && legacySetConstants != nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;

        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, "sl.interposer.dll") != 0)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            void* current = reinterpret_cast<void*>(static_cast<uintptr_t>(thunk->u1.Function));

            if (modernResolver &&
                (current == modernResolver || current == reinterpret_cast<void*>(&HookGetFeatureFunction))) {
                sawTarget = true;
                if (current == reinterpret_cast<void*>(&HookGetFeatureFunction))
                    continue;

                if (!g_realGetFeatureFunction)
                    g_realGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(modernResolver);

                if (!PatchImportSlot(thunk, reinterpret_cast<void*>(&HookGetFeatureFunction),
                        L"slGetFeatureFunction")) {
                    return false;
                }

                if (g_shared && InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
                        ReflexProbeProtocol::HookStateWaitingForDll) {
                    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateReflexFound);
                }
                continue;
            }

            if (useLegacyPath &&
                (current == legacySetConstants || current == reinterpret_cast<void*>(&HookLegacySetFeatureConstants))) {
                sawTarget = true;
                if (current == reinterpret_cast<void*>(&HookLegacySetFeatureConstants))
                    continue;

                if (!g_realLegacySetFeatureConstants) {
                    g_realLegacySetFeatureConstants =
                        reinterpret_cast<PFunLegacySetFeatureConstants*>(legacySetConstants);
                }

                if (!PatchImportSlot(thunk, reinterpret_cast<void*>(&HookLegacySetFeatureConstants),
                        L"slSetFeatureConstants")) {
                    return false;
                }

                PublishReflexFunction(legacySetConstants);
            }
        }
    }

    return true;
}

bool PatchLoadedStreamlineImports(bool& foundAny)
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
            HMODULE heldModule = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(entry.modBaseAddr), &heldModule)) {
                continue;
            }

            bool sawTarget = false;
            const bool patched = PatchStreamlineImportsInModule(heldModule, sawTarget);
            FreeLibrary(heldModule);

            if (!patched || (g_shared && InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
                    ReflexProbeProtocol::HookStateError)) {
                success = false;
                break;
            }

            if (sawTarget) {
                foundAny = true;
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

    // Patch one stable Streamline API boundary, then leave the game alone. Modern
    // Streamline uses slGetFeatureFunction -> slReflexSetOptions; 1.x uses the generic
    // slSetFeatureConstants(eFeatureReflex, ReflexConstants) API.
    for (uint32_t attempt = 0;; ++attempt) {
        bool foundStreamlineImport = false;
        if (!PatchLoadedStreamlineImports(foundStreamlineImport))
            return 1;

        if (foundStreamlineImport) {
            if (g_realLegacySetFeatureConstants) {
                OutputDebugStringW(
                    L"ReflexProbe64: intercepted legacy slSetFeatureConstants Reflex path; worker exiting.\n");
            } else {
                OutputDebugStringW(
                    L"ReflexProbe64: intercepted modern slGetFeatureFunction resolver; worker exiting.\n");
            }
            return 0;
        }

        Sleep(attempt < 100 ? 10 : 100);
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
