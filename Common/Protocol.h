#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdlib.h> // MSVC _countof

namespace ReflexProbeProtocol {

constexpr uint32_t kMagic = 0x31505246; // "FRP1" little-endian.
constexpr uint32_t kVersion = 16;
constexpr uint32_t kEventCapacity = 4096;
constexpr uint32_t kPathChars = 1024;
constexpr uint32_t kVersionChars = 64;
constexpr uint32_t kErrorChars = 512;

// Human-readable source tag for rapid local rebuild/testing. The controller also hashes the
// actual EXE and DLL on disk, so a stale or mismatched binary is obvious in copied logs.
constexpr wchar_t kBuildTag[] = L"2026-10-22.39-live-dxgi-overrides";

constexpr wchar_t kMappingPrefix[] = L"Local\\ReflexProbe.";

enum HookState : LONG {
    HookStateWaitingForDll = 0,
    HookStateReflexFound = 1,
    HookStateHooked = 2,
    HookStateError = 3,
    HookStateInterceptArmed = 4
};

enum ReflexBackend : LONG {
    ReflexBackendUnknown = 0,
    ReflexBackendModernSetOptions = 1,
    ReflexBackendLegacyFeatureConstants = 2,
    ReflexBackendLegacyPluginConstants = 3,
    ReflexBackendNativeNvapiD3D = 4
};

enum ReflexEventKind : LONG {
    ReflexEventSettings = 0,
    ReflexEventSleep = 1,
    FgEventSetOptions = 2,
    FgEventGetState = 3,
    PresentationEventSwapChain = 4,
    PresentationEventPresent = 5
};

// Only POD values cross the shared-memory boundary. FG-specific fields are
// meaningful only for FgEventSetOptions / FgEventGetState.
struct FgEventData {
    uint32_t viewport;
    uint32_t optionsVersion;
    uint32_t optionsPresent;
    uint32_t mode;
    uint32_t generatedFrames; // original game's SetOptions value
    uint32_t forwardedGeneratedFrames; // effective forwarded SetOptions value
    uint32_t flags;
    uint32_t forwardedMode; // forwarded FG SetOptions mode
    uint32_t forwardedFlags;
    uint32_t dynamicWidth;
    uint32_t dynamicHeight;
    uint32_t numBackBuffers;
    uint32_t motionDepthWidth;
    uint32_t motionDepthHeight;
    uint32_t colorWidth;
    uint32_t colorHeight;
    uint32_t colorBufferFormat;
    uint32_t motionBufferFormat;
    uint32_t depthBufferFormat;
    uint32_t hudlessBufferFormat;
    uint32_t uiBufferFormat;
    uint32_t errorCallbackPresent;
    uint32_t targetFrameRateBits; // float bit pattern, options v5+
    uint32_t uiRecomposition;     // options v4+
    uint32_t parallelism;         // options v3+
    uint32_t stateVersion;
    uint32_t stateValid;
    uint32_t status;
    uint32_t minDimension;
    uint32_t framesPresented;     // transient since prior GetState
    uint32_t maxGeneratedFrames;  // state v2+
    uint32_t vsyncAvailable;      // state v2+
    uint32_t dynamicMfgAvailable; // state v4+
};

// DXGI only in this experiment. Identifiers are opaque swap-chain addresses
// in the injected process, not COM pointers for the controller to dereference.
struct PresentationEventData {
    uint64_t swapChain;
    uint32_t method; // 0 = Present, 1 = Present1; creation method for SwapChain event.
    uint32_t syncInterval;
    uint32_t presentFlags; // game request
    uint32_t forwardedSyncInterval;
    uint32_t forwardedPresentFlags;
    uint32_t overrideNotes; // 1=no tearing swap-chain flag, 2=exclusive fullscreen, 4=GetDesc/FS query failure, 8=SyncInterval nonzero, 16=TEST call
    uint32_t width;
    uint32_t height;
    uint32_t buffers;
    uint32_t format;
    uint32_t swapEffect;
    uint32_t creationFlags;
};

struct ReflexEvent {
    volatile LONG sequence;
    LONGLONG qpc;
    LONG kind;
    LONG callSequence;
    LONG requestedMode;
    LONG effectiveMode;
    uint32_t requestedUs;
    uint32_t effectiveUs;
    LONG result;
    FgEventData fg;
    PresentationEventData presentation;
};

struct SharedState {
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    DWORD targetProcessId;

    volatile LONG configSequence;
    volatile LONG overrideEnabled;
    volatile LONG overrideUs;
    volatile LONG forceBoostWhenOn;
    LONG countReflexSleepCalls;
    // Feature selection is immutable for the lifetime of the acquired target.
    LONG probeReflexEnabled;
    LONG probeDlssFgEnabled;
    LONG probePresentationEnabled;
    LONG wrapFgGetState; // acquisition-time, original pointer when false
    volatile LONG fgForceOnWhenAuto;
    volatile LONG fgForceOffAfterOn; // enables Off override; mode selected below
    volatile LONG fgOffOverrideAlways; // 0=after successful active call, 1=every Off call
    volatile LONG fgMultiplierOverrideEnabled;
    volatile LONG fgMultiplierFrames; // generated frames: 1=2X through 5=6X
    volatile LONG fgMenuOverrideEnabled;
    volatile LONG fgMenuOverrideOn;
    volatile LONG fgRetentionOverrideEnabled;
    volatile LONG fgRetentionOverrideOn;
    volatile LONG presentationSyncOverrideEnabled;
    volatile LONG presentationSyncInterval; // 0-4
    volatile LONG presentationTearingOverrideEnabled;
    volatile LONG presentationTearingOverrideOn;

    volatile LONG hookState;
    volatile LONG backend;
    volatile LONG eventSerial;
    volatile LONG settingsCallSerial;
    volatile LONG sleepCallSerial;
    volatile LONG fgSetCallSerial;
    volatile LONG fgGetCallSerial;
    volatile LONG presentationCallSerial;
    volatile LONG presentationHookBits; // 1=factory, 2=Present, 4=Present1, 8=ResizeBuffers, 16=D3D11 helper, 32=factory recursion detected, 64=missing Present predecessor.
    volatile LONG fgHookBits; // 1 = SetOptions resolved; 2 = GetState resolved.

    wchar_t targetPath[kPathChars];
    wchar_t reflexPath[kPathChars];
    wchar_t reflexVersion[kVersionChars];
    wchar_t fgPath[kPathChars];
    wchar_t fgVersion[kVersionChars];
    wchar_t fgMethod[256];
    wchar_t injectedBuild[kVersionChars];
    wchar_t lastError[kErrorChars];

    ReflexEvent events[kEventCapacity];
};

} // namespace ReflexProbeProtocol
