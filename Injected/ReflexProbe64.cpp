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
#include <intrin.h>

#include <sl.h>
#include <sl_reflex.h>
#include <sl_dlss_g.h>

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

// Native D3D Reflex predates Streamline. NVAPI applications resolve entry points through
// nvapi_QueryInterface. NVIDIA's public interface table assigns SetSleepMode ID 0xac1ca9e0.
// We keep only the stable prefix we actually inspect/modify and copy the caller's complete
// versioned structure byte-for-byte before forwarding it.
constexpr uint32_t kNvapiD3DSetSleepModeId = 0xac1ca9e0u;
constexpr uint32_t kNvapiD3DSleepId = 0x852cd1d2u;
constexpr size_t kNvapiSleepModeMaxBytes = 256;

struct NvapiSleepModePrefix {
    uint32_t version;
    uint8_t lowLatencyMode;
    uint8_t lowLatencyBoost;
    uint8_t alignment[2];
    uint32_t minimumIntervalUs;
};

static_assert(offsetof(NvapiSleepModePrefix, version) == 0);
static_assert(offsetof(NvapiSleepModePrefix, lowLatencyMode) == 4);
static_assert(offsetof(NvapiSleepModePrefix, lowLatencyBoost) == 5);
static_assert(offsetof(NvapiSleepModePrefix, minimumIntervalUs) == 8);
static_assert(sizeof(NvapiSleepModePrefix) == 12);

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
using PFunNvapiQueryInterface = void* (__cdecl)(uint32_t interfaceId);
using PFunNvapiD3DSetSleepMode = int32_t (__cdecl)(void* device, void* params);
using PFunNvapiD3DSleep = int32_t (__cdecl)(void* device);

HANDLE g_mapping = nullptr;
ReflexProbeProtocol::SharedState* g_shared = nullptr;

// The selected feature families are immutable for the acquired target.
// Resolver interception is shared plumbing; disabled feature functions are untouched.
bool IsReflexProbingEnabled()
{
    return g_shared && g_shared->probeReflexEnabled != 0;
}

bool IsFgProbingEnabled()
{
    return g_shared && g_shared->probeDlssFgEnabled != 0;
}

PFun_slGetFeatureFunction* g_realGetFeatureFunction = nullptr;
PFun_slReflexSetOptions* g_realReflexSetOptions = nullptr;
PFun_slReflexSleep* g_realReflexSleep = nullptr;
PFun_slDLSSGSetOptions* g_realFgSetOptions = nullptr;
PFun_slDLSSGGetState* g_realFgGetState = nullptr;
PFunLegacySetFeatureConstants* g_realLegacySetFeatureConstants = nullptr;
PFunLegacyPluginGetFunction* g_realLegacyPluginGetFunction = nullptr;
PFunLegacyPluginSetConstants* g_realLegacyPluginSetConstants = nullptr;
PFunGetProcAddress* g_realGetProcAddress = nullptr;
PFunNvapiQueryInterface* g_realNvapiQueryInterface = nullptr;
PFunNvapiD3DSetSleepMode* g_realNvapiD3DSetSleepMode = nullptr;
PFunNvapiD3DSleep* g_realNvapiD3DSleep = nullptr;
volatile LONG g_modernReflexCaptured = 0;
volatile LONG g_nativeNvapiCaptured = 0;
volatile LONG g_legacyPluginGatewayArmed = 0;
wchar_t g_modernInterceptionMethodStorage[256]{};
const wchar_t* g_modernInterceptionMethod = L"modern slGetFeatureFunction";
const wchar_t* g_nvapiQueryInterceptionMethod =
    L"native NVAPI D3D SetSleepMode via application nvapi_QueryInterface IAT";

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

void SetModernInterceptionMethod(HMODULE callerModule, const wchar_t* route)
{
    wchar_t callerPath[ReflexProbeProtocol::kPathChars]{};
    const wchar_t* callerName = L"unknown module";
    if (callerModule && GetModulePath(callerModule, callerPath, _countof(callerPath)))
        callerName = PathFileName(callerPath);

    swprintf_s(g_modernInterceptionMethodStorage,
        L"modern slGetFeatureFunction via %s %s", callerName, route);
    g_modernInterceptionMethod = g_modernInterceptionMethodStorage;
}

void SetModernInterceptionMethodFromAddress(void* address, const wchar_t* route)
{
    HMODULE callerModule = nullptr;
    if (address) {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &callerModule);
    }

    SetModernInterceptionMethod(callerModule, route);
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

void PublishSettingsEvent(LONG requestedMode, LONG effectiveMode,
                          uint32_t requestedUs, uint32_t effectiveUs, LONG result)
{
    if (!g_shared)
        return;

    const LONG callSequence = InterlockedIncrement(&g_shared->settingsCallSerial);
    const LONG serial = InterlockedIncrement(&g_shared->eventSerial);
    const uint32_t index = static_cast<uint32_t>(serial - 1) % ReflexProbeProtocol::kEventCapacity;
    ReflexProbeProtocol::ReflexEvent& event = g_shared->events[index];

    InterlockedExchange(&event.sequence, 0);

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.kind = ReflexProbeProtocol::ReflexEventSettings;
    event.callSequence = callSequence;
    event.requestedMode = requestedMode;
    event.effectiveMode = effectiveMode;
    event.requestedUs = requestedUs;
    event.effectiveUs = effectiveUs;
    event.result = result;

    MemoryBarrier();
    InterlockedExchange(&event.sequence, serial);
}

void PublishSleepEvent(LONG result)
{
    if (!g_shared)
        return;

    const LONG sleepSequence = InterlockedIncrement(&g_shared->sleepCallSerial);
    const LONG serial = InterlockedIncrement(&g_shared->eventSerial);
    const uint32_t index =
        static_cast<uint32_t>(serial - 1) % ReflexProbeProtocol::kEventCapacity;
    ReflexProbeProtocol::ReflexEvent& event = g_shared->events[index];

    InterlockedExchange(&event.sequence, 0);

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.kind = ReflexProbeProtocol::ReflexEventSleep;
    event.callSequence = sleepSequence;
    event.requestedMode = 0;
    event.effectiveMode = 0;
    event.requestedUs = 0;
    event.effectiveUs = 0;
    event.result = result;

    MemoryBarrier();
    InterlockedExchange(&event.sequence, serial);
}

// FG settings are observational only. This is deliberately independent of the
// Reflex backend and its existing state machine.
void PublishFgEvent(LONG kind, const ReflexProbeProtocol::FgEventData& data, LONG result)
{
    if (!g_shared)
        return;

    const LONG callSequence = kind == ReflexProbeProtocol::FgEventSetOptions
        ? InterlockedIncrement(&g_shared->fgSetCallSerial)
        : InterlockedIncrement(&g_shared->fgGetCallSerial);
    const LONG serial = InterlockedIncrement(&g_shared->eventSerial);
    const uint32_t index = static_cast<uint32_t>(serial - 1) % ReflexProbeProtocol::kEventCapacity;
    ReflexProbeProtocol::ReflexEvent& event = g_shared->events[index];
    InterlockedExchange(&event.sequence, 0);

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.kind = kind;
    event.callSequence = callSequence;
    event.requestedMode = 0;
    event.effectiveMode = 0;
    event.requestedUs = 0;
    event.effectiveUs = 0;
    event.result = result;
    event.fg = data;

    MemoryBarrier();
    InterlockedExchange(&event.sequence, serial);
}

void CaptureFgOptions(const sl::DLSSGOptions& options, ReflexProbeProtocol::FgEventData& out)
{
    out.optionsPresent = 1;
    out.optionsVersion = static_cast<uint32_t>(options.structVersion);
    out.mode = static_cast<uint32_t>(options.mode);
    out.generatedFrames = options.numFramesToGenerate;
    out.flags = static_cast<uint32_t>(options.flags);
    out.dynamicWidth = options.dynamicResWidth;
    out.dynamicHeight = options.dynamicResHeight;
    out.numBackBuffers = options.numBackBuffers;
    out.motionDepthWidth = options.mvecDepthWidth;
    out.motionDepthHeight = options.mvecDepthHeight;
    out.colorWidth = options.colorWidth;
    out.colorHeight = options.colorHeight;
    out.colorBufferFormat = options.colorBufferFormat;
    out.motionBufferFormat = options.mvecBufferFormat;
    out.depthBufferFormat = options.depthBufferFormat;
    out.hudlessBufferFormat = options.hudLessBufferFormat;
    out.uiBufferFormat = options.uiBufferFormat;
    out.errorCallbackPresent = options.onErrorCallback ? 1u : 0u;
    // Members are appended by version. Never read newer fields from an older
    // caller allocation, even though this observer was compiled with a newer SDK.
    if (options.structVersion >= 3)
        out.parallelism = static_cast<uint32_t>(options.queueParallelismMode);
    if (options.structVersion >= 4)
        out.uiRecomposition = static_cast<uint32_t>(options.enableUserInterfaceRecomposition);
    if (options.structVersion >= 5)
        memcpy(&out.targetFrameRateBits, &options.dynamicTargetFrameRate,
            sizeof(out.targetFrameRateBits));
}

sl::Result HookFgSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    PFun_slDLSSGSetOptions* real = g_realFgSetOptions;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    ReflexProbeProtocol::FgEventData data{};
    data.viewport = static_cast<uint32_t>(viewport);
    CaptureFgOptions(options, data);
    const sl::Result result = real(viewport, options);
    PublishFgEvent(ReflexProbeProtocol::FgEventSetOptions, data, static_cast<LONG>(result));
    return result;
}

sl::Result HookFgGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                          const sl::DLSSGOptions* options)
{
    PFun_slDLSSGGetState* real = g_realFgGetState;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    ReflexProbeProtocol::FgEventData data{};
    data.viewport = static_cast<uint32_t>(viewport);
    // Record the version before forwarding; the game owns this output buffer.
    data.stateVersion = static_cast<uint32_t>(state.structVersion);
    if (options)
        CaptureFgOptions(*options, data);

    const sl::Result result = real(viewport, state, options);
    if (result == sl::Result::eOk) {
        data.stateValid = 1;
        data.status = static_cast<uint32_t>(state.status);
        data.minDimension = state.minWidthOrHeight;
        data.framesPresented = state.numFramesActuallyPresented;
        if (data.stateVersion >= 2) {
            data.maxGeneratedFrames = state.numFramesToGenerateMax;
            data.vsyncAvailable = static_cast<uint32_t>(state.bIsVsyncSupportAvailable);
        }
        if (data.stateVersion >= 4)
            data.dynamicMfgAvailable = static_cast<uint32_t>(state.bIsDynamicMFGSupported);
    }
    PublishFgEvent(ReflexProbeProtocol::FgEventGetState, data, static_cast<LONG>(result));
    return result;
}

void PublishFgFunction(void* function, LONG bit)
{
    if (!g_shared || !function)
        return;

    MEMORY_BASIC_INFORMATION memory{};
    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (VirtualQuery(function, &memory, sizeof(memory)) &&
        GetModulePath(reinterpret_cast<HMODULE>(memory.AllocationBase),
            modulePath, _countof(modulePath))) {
        wcsncpy_s(g_shared->fgPath, _countof(g_shared->fgPath), modulePath, _TRUNCATE);
        GetFileVersionString(modulePath, g_shared->fgVersion, _countof(g_shared->fgVersion));
    }
    wcsncpy_s(g_shared->fgMethod, _countof(g_shared->fgMethod),
        g_modernInterceptionMethod, _TRUNCATE);
    MemoryBarrier();
    InterlockedOr(&g_shared->fgHookBits, bit);
}

void PublishReflexModule(HMODULE module, ReflexProbeProtocol::HookState state,
                         const wchar_t* interceptionMethod)
{
    if (!g_shared || !module)
        return;

    const wchar_t* method =
        interceptionMethod && *interceptionMethod ? interceptionMethod : L"unknown";

    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)))
        wcscpy_s(modulePath, L"Reflex module");

    GetFileVersionString(modulePath, g_shared->reflexVersion,
        _countof(g_shared->reflexVersion));
    _snwprintf_s(g_shared->reflexPath, _countof(g_shared->reflexPath), _TRUNCATE,
        L"%s\r\nInterception method: %s", modulePath, method);

    MemoryBarrier();
    InterlockedExchange(&g_shared->hookState, static_cast<LONG>(state));
}

void PublishReflexFunction(void* function, const wchar_t* interceptionMethod)
{
    if (!g_shared || !function)
        return;

    const wchar_t* method =
        interceptionMethod && *interceptionMethod ? interceptionMethod : L"unknown";

    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(function, &memory, sizeof(memory))) {
        PublishReflexModule(reinterpret_cast<HMODULE>(memory.AllocationBase),
            ReflexProbeProtocol::HookStateHooked, method);
    } else {
        _snwprintf_s(g_shared->reflexPath, _countof(g_shared->reflexPath), _TRUNCATE,
            L"Reflex module\r\nInterception method: %s", method);
        g_shared->reflexVersion[0] = 0;
        MemoryBarrier();
        InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateHooked);
    }
}

LONG NvapiModeFromSleepParams(const NvapiSleepModePrefix& params)
{
    if (!params.lowLatencyMode)
        return 0;
    return params.lowLatencyBoost ? 2 : 1;
}

int32_t __cdecl HookNvapiD3DSetSleepMode(void* device, void* params)
{
    PFunNvapiD3DSetSleepMode* real = g_realNvapiD3DSetSleepMode;
    if (!real)
        return -4; // NVAPI_API_NOT_INITIALIZED

    if (!params)
        return real(device, params);

    const auto* requested = static_cast<const NvapiSleepModePrefix*>(params);
    const uint32_t structSize = requested->version & 0xffffu;
    if (structSize < sizeof(NvapiSleepModePrefix) || structSize > kNvapiSleepModeMaxBytes) {
        OutputDebugStringW(
            L"ReflexProbe64: NVAPI SetSleepMode structure size is outside the supported copy range; forwarding unchanged.\n");
        return real(device, params);
    }

    alignas(8) unsigned char forwardedBytes[kNvapiSleepModeMaxBytes]{};
    memcpy(forwardedBytes, params, structSize);
    auto* forwarded = reinterpret_cast<NvapiSleepModePrefix*>(forwardedBytes);

    const LONG requestedMode = NvapiModeFromSleepParams(*requested);
    if (g_shared &&
        InterlockedCompareExchange(&g_shared->forceBoostWhenOn, 0, 0) != 0 &&
        forwarded->lowLatencyMode && !forwarded->lowLatencyBoost) {
        forwarded->lowLatencyBoost = 1;
    }

    const LONG effectiveMode = NvapiModeFromSleepParams(*forwarded);
    const uint32_t effectiveUs = ApplyConfiguredOverride(requested->minimumIntervalUs);
    forwarded->minimumIntervalUs = effectiveUs;

    const int32_t result = real(device, forwarded);
    PublishSettingsEvent(requestedMode, effectiveMode,
        requested->minimumIntervalUs, effectiveUs, static_cast<LONG>(result));

    InterlockedExchange(&g_nativeNvapiCaptured, 1);
    return result;
}

int32_t __cdecl HookNvapiD3DSleep(void* device)
{
    PFunNvapiD3DSleep* real = g_realNvapiD3DSleep;
    if (!real)
        return -4; // NVAPI_API_NOT_INITIALIZED

    const int32_t result = real(device);
    PublishSleepEvent(static_cast<LONG>(result));
    return result;
}

void* __cdecl HookNvapiQueryInterface(uint32_t interfaceId)
{
    PFunNvapiQueryInterface* real = g_realNvapiQueryInterface;
    if (!real)
        return nullptr;

    void* function = real(interfaceId);
    if (!function)
        return function;

    if (interfaceId == kNvapiD3DSetSleepModeId && IsReflexProbingEnabled()) {
        g_realNvapiD3DSetSleepMode =
            reinterpret_cast<PFunNvapiD3DSetSleepMode*>(function);
        SetBackend(ReflexProbeProtocol::ReflexBackendNativeNvapiD3D);
        PublishReflexFunction(function, g_nvapiQueryInterceptionMethod);
        InterlockedExchange(&g_nativeNvapiCaptured, 1);
        return reinterpret_cast<void*>(&HookNvapiD3DSetSleepMode);
    }

    if (interfaceId == kNvapiD3DSleepId && IsReflexProbingEnabled() &&
        g_shared->countReflexSleepCalls) {
        g_realNvapiD3DSleep = reinterpret_cast<PFunNvapiD3DSleep*>(function);
        return reinterpret_cast<void*>(&HookNvapiD3DSleep);
    }

    return function;
}

sl::Result HookReflexSleep(const sl::FrameToken& frame)
{
    PFun_slReflexSleep* real = g_realReflexSleep;
    if (!real)
        return sl::Result::eErrorNotInitialized;

    const sl::Result result = real(frame);
    PublishSleepEvent(static_cast<LONG>(result));
    return result;
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
    PublishSettingsEvent(requestedMode, effectiveMode,
        options.frameLimitUs, effectiveUs, static_cast<LONG>(result));
    return result;
}

bool HookLegacySetFeatureConstants(uint32_t feature, const void* constants,
                                   uint32_t frameIndex, uint32_t id)
{
    PFunLegacySetFeatureConstants* real = g_realLegacySetFeatureConstants;
    if (!real)
        return false;

    if (!IsReflexProbingEnabled() || feature != kLegacyFeatureReflex || !constants)
        return real(feature, constants, frameIndex, id);

    const auto* requested = static_cast<const LegacyReflexConstants*>(constants);
    LegacyReflexConstants forwarded = *requested;
    const LONG requestedMode = static_cast<LONG>(requested->mode);
    const LONG effectiveMode = ApplyConfiguredModeOverride(requestedMode);
    const uint32_t effectiveUs = ApplyConfiguredOverride(requested->frameLimitUs);
    forwarded.mode = static_cast<int32_t>(effectiveMode);
    forwarded.frameLimitUs = effectiveUs;

    const bool result = real(feature, &forwarded, frameIndex, id);
    PublishSettingsEvent(requestedMode, effectiveMode, requested->frameLimitUs,
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
    PublishSettingsEvent(requestedMode, effectiveMode, requested->frameLimitUs,
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

    if (IsReflexProbingEnabled() && strcmp(functionName, "slSetConstants") == 0) {
        g_realLegacyPluginSetConstants =
            reinterpret_cast<PFunLegacyPluginSetConstants*>(function);
        SetBackend(ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
        PublishReflexFunction(function,
            L"SL1 slGetPluginFunction via interposer GetProcAddress IAT");
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

    if (!IsReflexProbingEnabled() || strcmp(procName, "slGetPluginFunction") != 0)
        return result;

    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)) ||
        _wcsicmp(PathFileName(modulePath), L"sl.reflex.dll") != 0) {
        return result;
    }

    g_realLegacyPluginGetFunction = reinterpret_cast<PFunLegacyPluginGetFunction*>(result);
    SetBackend(ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
    PublishReflexModule(module, ReflexProbeProtocol::HookStateReflexFound,
        L"SL1 slGetPluginFunction via interposer GetProcAddress IAT");
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

    if (IsFgProbingEnabled() && feature == sl::kFeatureDLSS_G &&
        strcmp(functionName, "slDLSSGSetOptions") == 0) {
        g_realFgSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(function);
        PublishFgFunction(function, 1);
        function = reinterpret_cast<void*>(&HookFgSetOptions);
    } else if (IsFgProbingEnabled() && feature == sl::kFeatureDLSS_G &&
               strcmp(functionName, "slDLSSGGetState") == 0) {
        g_realFgGetState = reinterpret_cast<PFun_slDLSSGGetState*>(function);
        PublishFgFunction(function, 2);
        function = reinterpret_cast<void*>(&HookFgGetState);
    } else if (IsReflexProbingEnabled() && strcmp(functionName, "slReflexSetOptions") == 0) {
        g_realReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(function);
        SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
        PublishReflexFunction(function, g_modernInterceptionMethod);
        function = reinterpret_cast<void*>(&HookReflexSetOptions);
        InterlockedExchange(&g_modernReflexCaptured, 1);
    } else if (IsReflexProbingEnabled() && strcmp(functionName, "slReflexSleep") == 0 &&
               g_shared->countReflexSleepCalls) {
        g_realReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(function);
        function = reinterpret_cast<void*>(&HookReflexSleep);
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

    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)))
        return result;

    const wchar_t* moduleName = PathFileName(modulePath);

    if (IsReflexProbingEnabled() && strcmp(procName, "nvapi_QueryInterface") == 0 &&
        _wcsicmp(moduleName, L"nvapi64.dll") == 0) {
        g_realNvapiQueryInterface = reinterpret_cast<PFunNvapiQueryInterface*>(result);
        g_nvapiQueryInterceptionMethod =
            L"native NVAPI D3D SetSleepMode via application GetProcAddress IAT";
        return reinterpret_cast<FARPROC>(&HookNvapiQueryInterface);
    }

    if (IsReflexProbingEnabled() && strcmp(procName, "NvAPI_D3D_Sleep") == 0 &&
        _wcsicmp(moduleName, L"nvapi64.dll") == 0 &&
        g_shared->countReflexSleepCalls) {
        g_realNvapiD3DSleep = reinterpret_cast<PFunNvapiD3DSleep*>(result);
        return reinterpret_cast<FARPROC>(&HookNvapiD3DSleep);
    }

    if (strcmp(procName, "slGetFeatureFunction") != 0 ||
        _wcsicmp(moduleName, L"sl.interposer.dll") != 0) {
        return result;
    }

    g_realGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(result);
    SetModernInterceptionMethodFromAddress(_ReturnAddress(), L"GetProcAddress IAT");
    if (IsReflexProbingEnabled()) {
        SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
        PublishReflexModule(module, ReflexProbeProtocol::HookStateReflexFound,
            g_modernInterceptionMethod);
    }
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

bool PatchApplicationNvapiResolver(bool& armedAny)
{
    armedAny = false;

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module)
        return true;

    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return true;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return true;
    }

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return true;

    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    void* expectedQuery = nvapi
        ? reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface"))
        : nullptr;
    void* expectedSetSleepMode = nvapi
        ? reinterpret_cast<void*>(GetProcAddress(nvapi, "NvAPI_D3D_SetSleepMode"))
        : nullptr;
    void* expectedSleep = nvapi
        ? reinterpret_cast<void*>(GetProcAddress(nvapi, "NvAPI_D3D_Sleep"))
        : nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;

        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, "nvapi64.dll") != 0)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk)
            : nullptr;

        for (size_t index = 0; thunk[index].u1.Function; ++index) {
            void* current =
                reinterpret_cast<void*>(static_cast<uintptr_t>(thunk[index].u1.Function));
            const char* importedName = nullptr;

            if (names && names[index].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) {
                auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + names[index].u1.AddressOfData);
                importedName = reinterpret_cast<const char*>(imported->Name);
            }

            const bool queryMatches =
                (importedName && strcmp(importedName, "nvapi_QueryInterface") == 0) ||
                (expectedQuery && current == expectedQuery);
            if (queryMatches) {
                armedAny = true;
                if (current != reinterpret_cast<void*>(&HookNvapiQueryInterface)) {
                    g_realNvapiQueryInterface =
                        reinterpret_cast<PFunNvapiQueryInterface*>(current);
                    g_nvapiQueryInterceptionMethod =
                        L"native NVAPI D3D SetSleepMode via application nvapi_QueryInterface IAT";
                    if (!PatchImportSlot(&thunk[index],
                            reinterpret_cast<void*>(&HookNvapiQueryInterface),
                            L"nvapi_QueryInterface (native NVAPI Reflex resolver)")) {
                        return false;
                    }
                }
                continue;
            }

            const bool setSleepModeMatches =
                (importedName && strcmp(importedName, "NvAPI_D3D_SetSleepMode") == 0) ||
                (expectedSetSleepMode && current == expectedSetSleepMode);
            if (setSleepModeMatches) {
                armedAny = true;
                if (current != reinterpret_cast<void*>(&HookNvapiD3DSetSleepMode)) {
                    g_realNvapiD3DSetSleepMode =
                        reinterpret_cast<PFunNvapiD3DSetSleepMode*>(current);
                    if (!PatchImportSlot(&thunk[index],
                            reinterpret_cast<void*>(&HookNvapiD3DSetSleepMode),
                            L"NvAPI_D3D_SetSleepMode")) {
                        return false;
                    }
                    SetBackend(ReflexProbeProtocol::ReflexBackendNativeNvapiD3D);
                    PublishReflexFunction(current,
                        L"native NVAPI D3D SetSleepMode via application IAT");
                }
            }

            const bool sleepMatches =
                (importedName && strcmp(importedName, "NvAPI_D3D_Sleep") == 0) ||
                (expectedSleep && current == expectedSleep);
            if (sleepMatches && g_shared && g_shared->countReflexSleepCalls) {
                armedAny = true;
                if (current != reinterpret_cast<void*>(&HookNvapiD3DSleep)) {
                    g_realNvapiD3DSleep =
                        reinterpret_cast<PFunNvapiD3DSleep*>(current);
                    if (!PatchImportSlot(&thunk[index],
                            reinterpret_cast<void*>(&HookNvapiD3DSleep),
                            L"NvAPI_D3D_Sleep")) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool IsPathUnderWindowsDirectory(const wchar_t* path)
{
    if (!path || !path[0])
        return false;

    wchar_t windowsDirectory[MAX_PATH]{};
    const UINT chars = GetWindowsDirectoryW(
        windowsDirectory, static_cast<UINT>(_countof(windowsDirectory)));
    if (!chars || chars >= _countof(windowsDirectory))
        return false;

    const size_t length = wcslen(windowsDirectory);
    if (_wcsnicmp(path, windowsDirectory, length) != 0)
        return false;

    const wchar_t boundary = path[length];
    return boundary == 0 || boundary == L'\\' || boundary == L'/';
}

bool ShouldPatchGetProcAddressResolver(HMODULE module)
{
    wchar_t modulePath[ReflexProbeProtocol::kPathChars]{};
    if (!GetModulePath(module, modulePath, _countof(modulePath)))
        return false;

    const wchar_t* moduleName = PathFileName(modulePath);

    // The SL1 interposer has its own plugin-gateway hook. Do not replace that
    // GetProcAddress slot with the modern/application resolver hook.
    if (_wcsicmp(moduleName, L"sl.interposer.dll") == 0 ||
        _wcsicmp(moduleName, L"ReflexProbe64.dll") == 0) {
        return false;
    }

    // System DLLs do not originate application Streamline feature lookups. Keeping
    // their IATs untouched also limits this discovery hook to game/engine/plugin code.
    if (IsPathUnderWindowsDirectory(modulePath))
        return false;

    return true;
}

bool PatchGetProcAddressResolverInModule(HMODULE module)
{
    if (!module || !ShouldPatchGetProcAddressResolver(module))
        return true;

    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return true;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return true;
    }

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return true;

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* expectedGetProcAddress = kernel32
        ? reinterpret_cast<void*>(GetProcAddress(kernel32, "GetProcAddress"))
        : nullptr;
    if (!expectedGetProcAddress)
        return true;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk)
            : nullptr;

        for (size_t index = 0; thunk[index].u1.Function; ++index) {
            void* current =
                reinterpret_cast<void*>(static_cast<uintptr_t>(thunk[index].u1.Function));
            bool nameMatches = false;

            if (names && names[index].u1.AddressOfData &&
                !IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) {
                auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + names[index].u1.AddressOfData);
                nameMatches =
                    strcmp(reinterpret_cast<const char*>(imported->Name), "GetProcAddress") == 0;
            }

            if (!nameMatches && current != expectedGetProcAddress &&
                current != reinterpret_cast<void*>(&HookApplicationGetProcAddress)) {
                continue;
            }

            if (current == reinterpret_cast<void*>(&HookApplicationGetProcAddress))
                continue;

            // Do not trample another injector/overlay that already replaced this import.
            // All slots we own should still contain the canonical Kernel32 resolver.
            if (current != expectedGetProcAddress)
                continue;

            if (!g_realGetProcAddress) {
                g_realGetProcAddress =
                    reinterpret_cast<PFunGetProcAddress*>(expectedGetProcAddress);
            }

            if (!PatchImportSlot(&thunk[index],
                    reinterpret_cast<void*>(&HookApplicationGetProcAddress),
                    L"GetProcAddress (Reflex resolver)")) {
                return false;
            }
        }
    }

    return true;
}

bool PatchLegacyPluginGatewayResolver(HMODULE module, bool& sawTarget)
{
    sawTarget = false;

    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    if (!IsReflexProbingEnabled() || !interposer || module != interposer ||
        !IsLegacyStreamlineInterposer(interposer))
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
            if (!PatchImportSlot(&thunk[index], reinterpret_cast<void*>(&HookInterposerGetProcAddress),
                    L"GetProcAddress (SL1 plugin gateway)")) {
                return false;
            }

            SetBackend(ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
            InterlockedExchange(&g_legacyPluginGatewayArmed, 1);
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
    const bool useLegacyPublicPath = IsReflexProbingEnabled() &&
        IsLegacyStreamlineInterposer(interposer) && legacySetConstants != nullptr;

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

                SetModernInterceptionMethod(module, L"direct IAT");
                if (!PatchImportSlot(thunk, reinterpret_cast<void*>(&HookGetFeatureFunction),
                        L"slGetFeatureFunction")) {
                    return false;
                }

                if (IsReflexProbingEnabled())
                    SetBackend(ReflexProbeProtocol::ReflexBackendModernSetOptions);
                if (IsReflexProbingEnabled() &&
                    InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
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

                if (!PatchImportSlot(thunk, reinterpret_cast<void*>(&HookLegacySetFeatureConstants),
                        L"slSetFeatureConstants")) {
                    return false;
                }

                SetBackend(ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants);
                PublishReflexFunction(legacySetConstants,
                    L"SL1 slSetFeatureConstants IAT");
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
            bool patched = PatchStreamlineImportsInModule(heldModule, sawTarget);
            // A module can mix direct imports and dynamic lookups. In an FG
            // acquisition, cover both, rather than letting a Reflex IAT import
            // suppress discovery of that module's FG GetProcAddress path.
            if (patched && (!sawTarget || IsFgProbingEnabled()))
                patched = PatchGetProcAddressResolverInModule(heldModule);
            FreeLibrary(heldModule);

            if (!patched || (g_shared && InterlockedCompareExchange(&g_shared->hookState, 0, 0) ==
                    ReflexProbeProtocol::HookStateError)) {
                success = false;
                break;
            }

            if (sawTarget) {
                foundAny = true;
                // With FG enabled, keep discovering resolver imports in other
                // modules; the FG and Reflex call sites may be in different DLLs.
                if (!IsFgProbingEnabled())
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

    if (!IsReflexProbingEnabled() && !IsFgProbingEnabled())
        return 0;

    InterlockedExchange(&g_shared->hookState, ReflexProbeProtocol::HookStateWaitingForDll);

    // Prefer already-loaded/static Streamline boundaries. Also arm GetProcAddress IATs
    // across loaded non-system game/engine/plugin modules so dynamically resolved
    // slGetFeatureFunction calls are visible even when they originate outside the EXE.
    for (uint32_t attempt = 0;; ++attempt) {
        bool foundStreamlineImport = false;
        if (!PatchLoadedStreamlineImports(foundStreamlineImport))
            return 1;

        if (foundStreamlineImport && !IsFgProbingEnabled()) {
            if (InterlockedCompareExchange(&g_legacyPluginGatewayArmed, 0, 0) != 0) {
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

        bool armedNvapiResolver = false;
        if (IsReflexProbingEnabled() && !PatchApplicationNvapiResolver(armedNvapiResolver))
            return 1;

        if (!IsFgProbingEnabled() &&
            InterlockedCompareExchange(&g_nativeNvapiCaptured, 0, 0) != 0) {
            OutputDebugStringW(
                L"ReflexProbe64: captured native NVAPI D3D NvAPI_D3D_SetSleepMode; worker exiting.\n");
            return 0;
        }

        // Arming a GetProcAddress IAT is only a pending discovery path. Do not stop
        // scanning merely because one or more loaded modules have been patched.
        // Streamline 1.x titles such as A Plague Tale: Requiem can load sl.interposer.dll later,
        // and their Reflex path lives behind the interposer's own
        // slGetPluginFunction("slSetConstants") gateway.
        if (!IsFgProbingEnabled() &&
            InterlockedCompareExchange(&g_modernReflexCaptured, 0, 0) != 0) {
            OutputDebugStringW(
                L"ReflexProbe64: dynamic modern Reflex resolver captured slReflexSetOptions; worker exiting.\n");
            return 0;
        }

        if (IsFgProbingEnabled() &&
            (InterlockedCompareExchange(&g_shared->fgHookBits, 0, 0) & 3) == 3 &&
            (!IsReflexProbingEnabled() ||
             InterlockedCompareExchange(&g_modernReflexCaptured, 0, 0) ||
             InterlockedCompareExchange(&g_nativeNvapiCaptured, 0, 0) ||
             InterlockedCompareExchange(&g_legacyPluginGatewayArmed, 0, 0) ||
             g_realLegacySetFeatureConstants)) {
            OutputDebugStringW(L"ReflexProbe64: selected feature function paths resolved.\n");
            return 0;
        }

        // Keep discovery alive for later-loaded FG modules without polling
        // a fully running game's module list at 10 Hz indefinitely.
        Sleep(attempt < 100 ? 10 :
            (IsFgProbingEnabled() ? (attempt < 300 ? 250 : 1000) : 100));
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