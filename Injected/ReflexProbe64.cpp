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

#define RP_WIDEN_INNER(x) L##x
#define RP_WIDEN(x) RP_WIDEN_INNER(x)

namespace {

constexpr wchar_t kInjectedBuildStamp[] = RP_WIDEN(__DATE__) L" " RP_WIDEN(__TIME__);

// Streamline 1.x configured Reflex through ReflexConstants. The public app-facing API
// changed over the lifetime of SL1, but the sl.reflex plugin itself consumes this stable
// payload through its private slSetConstants function.
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
using PFunLegacyPluginGetFunction = void* (const char* functionName);
using PFunLegacyPluginSetConstants = bool(const void* constants, uint32_t frameIndex, uint32_t id);
using PFunGetProcAddress = FARPROC (WINAPI)(HMODULE module, LPCSTR procName);

HANDLE g_mapping = nullptr;
ReflexProbeProtocol::SharedState* g_shared = nullptr;
PFun_slGetFeatureFunction* g_realGetFeatureFunction = nullptr;
PFun_slReflexSetOptions* g_realReflexSetOptions = nullptr;
PFunLegacySetFeatureConstants* g_realLegacySetFeatureConstants = nullptr;
PFunLegacyPluginGetFunction* g_realLegacyPluginGetFunction = nullptr;
PFunLegacyPluginSetConstants* g_realLegacyPluginSetConstants = nullptr;
PFunGetProcAddress* g_realGetProcAddress = nullptr;
volatile LONG g_modernReflexCaptured = 0;
const wchar_t* g_interceptionMethod = L"unknown";

void SetInterceptionMethod(const wchar_t* method)
{
    g_interceptionMethod = method && *method ? method : L"unknown";
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

    wcsncpy_s(g_shared->injectedBuild, _countof(g_shared->injectedBuild),
        kInjectedBuildStamp, _TRUNCATE);
    MemoryBarrier();
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

bool GetModulePath(HMODULE module, wchar_t* out, size_t outCount)
{
    if (!module || !out || !outCount)
        return false;

    const DWORD count = GetModuleFileNameW(module, out, static_cast<DWORD>(outCount));
    if (!count || count >= outCount) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool IsLegacyStreamlineInterposer(HMODULE interposer)
{
    if (!interposer)
        return false;

    wchar_t path[ReflexProbeProtocol::kPathChars]{};
    wchar_t version[ReflexProbeProtocol::kVersionChars]{};
    if (GetModulePath(interposer, path, _countof(path))) {
        GetFileVersionString(path, version, _countof(version));
        if (version[0]) {
            wchar_t* end = nullptr;
            const unsigned long major = wcstoul(version, &end, 10);
            if (end != version && major == 1)
                return true;
            if (end != version && major >= 2)
                return false;
        }
    }

    // Some shipped Streamline DLLs carry unhelpful product file versions. The modern
    // public feature resolver is a stronger discriminator when version metadata is absent.
    return GetProcAddress(interposer, "slGetFeatureFunction") == nullptr;
}

void SetBackend(ReflexProbeProtocol::ReflexBackend backend)
{
    if (g_shared)
        InterlockedExchange(&g_shared->backend, static_cast<LONG>(backend));
}

uint32_t ApplyConfiguredOverride(uint32_t requestedUs)
{
    if (!g_shared || InterlockedCompareExchange(&g_shared->overrideEnabled, 0, 0) == 0)
        return requestedUs;

    const LONG configured = InterlockedCompareExchange(&g_shared->overrideUs, 0, 0);
    return static_cast<uint32_t>(configured);
}

LONG ApplyConfiguredModeOverride(LONG requestedMode)
{
    if (!g_shared || InterlockedCompareExchange(&g_shared->forceBoostWhenOn, 0, 0) == 0)
        return requestedMode;

    return requestedMode == 1 ? 2 : requestedMode;
}

void PublishEvent(LONG requestedMode, LONG effectiveMode,
                  uint32_t requestedUs, uint32_t effectiveUs, LONG result)
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
    event.requestedMode = requestedMode;
    event.effectiveMode = effectiveMode;
    event.requestedUs = requestedUs;
    event.effectiveUs = effectiveUs;
    event.result = result;

    MemoryBarrier();
    InterlockedExchange(&event.sequence, serial);
}

void PublishReflexModule(HMODULE module, ReflexProbeProtocol::HookState state)
{
    if (!g_shared || !module)
        return;

    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)))
        wcscpy_s(modulePath, L"Streamline module");

    GetFileVersionString(modulePath, g_shared->reflexVersion,
        _countof(g_shared->reflexVersion));
    _snwprintf_s(g_shared->reflexPath, _countof(g_shared->reflexPath), _TRUNCATE,
        L"%s\r\nInterception method: %s", modulePath,
        g_interceptionMethod ? g_interceptionMethod : L"unknown");

    MemoryBarrier();
    InterlockedExchange(&g_shared->hookState, static_cast<LONG>(state));
}

void PublishReflexFunction(void* function)
{
    if (!g_shared || !function)
        return;

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(function, &memory, sizeof(memory))) {
        PublishReflexModule(reinterpret_cast<HMODULE>(memory.AllocationBase),
            ReflexProbeProtocol::HookStateHooked);
    } else {
        _snwprintf_s(g_shared->reflexPath, _countof(g_shared->reflexPath), _TRUNCATE,
            L"Streamline module\r\nInterception method: %s",
            g_interceptionMethod ? g_interceptionMethod : L"unknown");
        g_shared->reflexVersion[0] = 0;
        MemoryBarrier();
        InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateHooked);
    }
}

sl::Result HookReflexSetOptions(const sl::ReflexOptions& options)
{
    PFun_slReflexSetOptions* real = g_realReflexSetOptions;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    sl::ReflexOptions forwarded = options;
    const LONG requestedMode = static_cast<LONG>(options.mode);
    const LONG effectiveMode = ApplyConfiguredModeOverride(requestedMode);
    const uint32_t effectiveUs = ApplyConfiguredOverride(options.frameLimitUs);
    forwarded.mode = static_cast<sl::ReflexMode>(effectiveMode);
    forwarded.frameLimitUs = effectiveUs;

    const sl::Result result = real(forwarded);
    PublishEvent(requestedMode, effectiveMode,
        options.frameLimitUs, effectiveUs, static_cast<LONG>(result));
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
    const LONG requestedMode = static_cast<LONG>(requested->mode);
    const LONG effectiveMode = ApplyConfiguredModeOverride(requestedMode);
    const uint32_t effectiveUs = ApplyConfiguredOverride(requested->frameLimitUs);
    forwarded.mode = static_cast<int32_t>(effectiveMode);
    forwarded.frameLimitUs = effectiveUs;

    const bool result = real(feature, &forwarded, frameIndex, id);
    PublishEvent(requestedMode, effectiveMode, requested->frameLimitUs,
        effectiveUs, result ? 0 : 1);
    return result;
}

bool HookLegacyPluginSetConstants(const void* constants, uint32_t frameIndex, uint32_t id)
{
    PFunLegacyPluginSetConstants* real = g_realLegacyPluginSetConstants;
    if (!real)
        return false;

    if (!constants)
        return real(constants, frameIndex, id);

    const auto* requested = static_cast<const LegacyReflexConstants*>(constants);
    LegacyReflexConstants forwarded = *requested;
    const LONG requestedMode = static_cast<LONG>(requested->mode);
    const LONG effectiveMode = ApplyConfiguredModeOverride(requestedMode);
    const uint32_t effectiveUs = ApplyConfiguredOverride(requested->frameLimitUs);
    forwarded.mode = static_cast<int32_t>(effectiveMode);
    forwarded.frameLimitUs = effectiveUs;

    const bool result = real(&forwarded, frameIndex, id);
    PublishEvent(requestedMode, effectiveMode, requested->frameLimitUs,
        effectiveUs, result ? 0 : 1);
    return result;
}

void* HookLegacyPluginGetFunction(const char* functionName)
{
    PFunLegacyPluginGetFunction* real = g_realLegacyPluginGetFunction;
    if (!real)
        return nullptr;

    void* function = real(functionName);
    if (!functionName || !function)
        return function;

    if (strcmp(functionName, "slSetConstants") == 0) {
        g_realLegacyPluginSetConstants =
            reinterpret_cast<PFunLegacyPluginSetConstants*>(function);
        SetInterceptionMethod(L"SL1 slGetPluginFunction via interposer GetProcAddress IAT");
        SetBackend(ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
        PublishReflexFunction(function);
        return reinterpret_cast<void*>(&HookLegacyPluginSetConstants);
    }

    return function;
}

FARPROC WINAPI HookInterposerGetProcAddress(HMODULE module, LPCSTR procName)
{
    PFunGetProcAddress* real = g_realGetProcAddress;
    if (!real)
        return nullptr;

    FARPROC result = real(module, procName);
    if (!result || !module || !procName)
        return result;

    // GetProcAddress also accepts ordinals encoded as small pointer values.
    if (reinterpret_cast<uintptr_t>(procName) <= 0xFFFFu)
        return result;

    if (strcmp(procName, "slGetPluginFunction") != 0)
        return result;

    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)) ||
        _wcsicmp(PathFileName(modulePath), L"sl.reflex.dll") != 0) {
        return result;
    }

    g_realLegacyPluginGetFunction = reinterpret_cast<PFunLegacyPluginGetFunction*>(result);
    SetInterceptionMethod(L"SL1 slGetPluginFunction via interposer GetProcAddress IAT");
    SetBackend(ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
    PublishReflexModule(module, ReflexProbeProtocol::HookStateReflexFound);
    return reinterpret_cast<FARPROC>(&HookLegacyPluginGetFunction);
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
        SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
        PublishReflexFunction(function);
        function = reinterpret_cast<void*>(&HookReflexSetOptions);
        InterlockedExchange(&g_modernReflexCaptured, 1);
    }

    return result;
}

FARPROC WINAPI HookApplicationGetProcAddress(HMODULE module, LPCSTR procName)
{
    PFunGetProcAddress* real = g_realGetProcAddress;
    if (!real)
        return nullptr;

    FARPROC result = real(module, procName);
    if (!result || !module || !procName)
        return result;

    if (reinterpret_cast<uintptr_t>(procName) <= 0xFFFFu)
        return result;
    if (strcmp(procName, "slGetFeatureFunction") != 0)
        return result;

    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)) ||
        _wcsicmp(PathFileName(modulePath), L"sl.interposer.dll") != 0) {
        return result;
    }

    g_realGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(result);
    SetInterceptionMethod(L"modern slGetFeatureFunction via application GetProcAddress IAT");
    SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
    PublishReflexModule(module, ReflexProbeProtocol::HookStateReflexFound);
    return reinterpret_cast<FARPROC>(&HookGetFeatureFunction);
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

bool PatchApplicationGetProcAddressResolver(bool& sawTarget)
{
    sawTarget = false;

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module)
        return true;

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

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* expectedGetProcAddress = kernel32
        ? reinterpret_cast<void*>(GetProcAddress(kernel32, "GetProcAddress"))
        : nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk)
            : nullptr;

        for (size_t index = 0; thunk[index].u1.Function; ++index) {
            void* current = reinterpret_cast<void*>(static_cast<uintptr_t>(thunk[index].u1.Function));
            bool nameMatches = false;

            if (names && names[index].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) {
                auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + names[index].u1.AddressOfData);
                nameMatches = strcmp(reinterpret_cast<const char*>(imported->Name), "GetProcAddress") == 0;
            } else if (expectedGetProcAddress) {
                nameMatches = current == expectedGetProcAddress;
            }

            if (!nameMatches)
                continue;

            sawTarget = true;
            if (current == reinterpret_cast<void*>(&HookApplicationGetProcAddress))
                return true;

            g_realGetProcAddress = reinterpret_cast<PFunGetProcAddress*>(current);
            SetInterceptionMethod(L"modern slGetFeatureFunction via application GetProcAddress IAT");
            if (!PatchImportSlot(&thunk[index], reinterpret_cast<void*>(&HookApplicationGetProcAddress),
                    L"GetProcAddress (modern Streamline resolver)")) {
                return false;
            }

            SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
            return true;
        }
    }

    return true;
}

bool PatchLegacyPluginGatewayResolver(HMODULE module, bool& sawTarget)
{
    sawTarget = false;

    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    if (!interposer || module != interposer || !IsLegacyStreamlineInterposer(interposer))
        return true;

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

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* expectedGetProcAddress = kernel32
        ? reinterpret_cast<void*>(GetProcAddress(kernel32, "GetProcAddress"))
        : nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk)
            : nullptr;

        for (size_t index = 0; thunk[index].u1.Function; ++index) {
            void* current = reinterpret_cast<void*>(static_cast<uintptr_t>(thunk[index].u1.Function));
            bool nameMatches = false;

            if (names && names[index].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) {
                auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + names[index].u1.AddressOfData);
                nameMatches = strcmp(reinterpret_cast<const char*>(imported->Name), "GetProcAddress") == 0;
            } else if (expectedGetProcAddress) {
                nameMatches = current == expectedGetProcAddress;
            }

            if (!nameMatches)
                continue;

            sawTarget = true;
            if (current == reinterpret_cast<void*>(&HookInterposerGetProcAddress))
                return true;

            g_realGetProcAddress = reinterpret_cast<PFunGetProcAddress*>(current);
            SetInterceptionMethod(L"SL1 slGetPluginFunction via interposer GetProcAddress IAT");
            if (!PatchImportSlot(&thunk[index], reinterpret_cast<void*>(&HookInterposerGetProcAddress),
                    L"GetProcAddress (SL1 plugin gateway)")) {
                return false;
            }

            SetBackend(ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
            if (g_shared)
                InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateInterceptArmed);
            return true;
        }
    }

    return true;
}

bool PatchStreamlineImportsInModule(HMODULE module, bool& sawTarget)
{
    sawTarget = false;
    if (!module)
        return false;

    bool sawLegacyGateway = false;
    if (!PatchLegacyPluginGatewayResolver(module, sawLegacyGateway))
        return false;
    if (sawLegacyGateway) {
        sawTarget = true;
        return true;
    }

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
    const bool useLegacyPublicPath = IsLegacyStreamlineInterposer(interposer) && legacySetConstants != nullptr;

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

                SetInterceptionMethod(L"modern slGetFeatureFunction IAT");
                if (!PatchImportSlot(thunk, reinterpret_cast<void*>(&HookGetFeatureFunction),
                        L"slGetFeatureFunction")) {
                    return false;
                }

                SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
                if (g_shared && InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
                        ReflexProbeProtocol::HookStateWaitingForDll) {
                    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateReflexFound);
                }
                continue;
            }

            if (useLegacyPublicPath &&
                (current == legacySetConstants || current == reinterpret_cast<void*>(&HookLegacySetFeatureConstants))) {
                sawTarget = true;
                if (current == reinterpret_cast<void*>(&HookLegacySetFeatureConstants))
                    continue;

                if (!g_realLegacySetFeatureConstants) {
                    g_realLegacySetFeatureConstants =
                        reinterpret_cast<PFunLegacySetFeatureConstants*>(legacySetConstants);
                }

                SetInterceptionMethod(L"SL1 slSetFeatureConstants IAT");
                if (!PatchImportSlot(thunk, reinterpret_cast<void*>(&HookLegacySetFeatureConstants),
                        L"slSetFeatureConstants")) {
                    return false;
                }

                SetBackend(ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants);
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

    // Prefer already-loaded/static Streamline boundaries. If the application loads the
    // interposer dynamically, arm only the main executable's GetProcAddress IAT so we can
    // substitute slGetFeatureFunction when Streamline is resolved later.
    for (uint32_t attempt = 0;; ++attempt) {
        bool foundStreamlineImport = false;
        if (!PatchLoadedStreamlineImports(foundStreamlineImport))
            return 1;

        if (foundStreamlineImport) {
            if (g_realGetProcAddress) {
                OutputDebugStringW(
                    L"ReflexProbe64: armed Streamline 1.x sl.reflex plugin-gateway interception; worker exiting.\n");
            } else if (g_realLegacySetFeatureConstants) {
                OutputDebugStringW(
                    L"ReflexProbe64: intercepted legacy slSetFeatureConstants Reflex path; worker exiting.\n");
            } else {
                OutputDebugStringW(
                    L"ReflexProbe64: intercepted modern slGetFeatureFunction resolver; worker exiting.\n");
            }
            return 0;
        }

        bool armedDynamicResolver = false;
        if (!PatchApplicationGetProcAddressResolver(armedDynamicResolver))
            return 1;

        // Arming the application's GetProcAddress IAT is only a pending modern
        // discovery path. Do not stop scanning loaded modules here: Streamline 1.x
        // titles such as A Plague Tale: Requiem can load sl.interposer.dll later,
        // and their Reflex path lives behind the interposer's own
        // slGetPluginFunction("slSetConstants") gateway.
        if (InterlockedCompareExchange(&g_modernReflexCaptured, 0, 0) != 0) {
            OutputDebugStringW(
                L"ReflexProbe64: dynamic modern Reflex resolver captured slReflexSetOptions; worker exiting.\n");
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

#undef RP_WIDEN
#undef RP_WIDEN_INNER