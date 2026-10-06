#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdlib.h> // MSVC _countof

namespace ReflexProbeProtocol {

constexpr uint32_t kMagic = 0x31505246; // "FRP1" little-endian.
constexpr uint32_t kVersion = 3;
constexpr uint32_t kEventCapacity = 4096;
constexpr uint32_t kPathChars = 1024;
constexpr uint32_t kVersionChars = 64;
constexpr uint32_t kErrorChars = 512;

// Human-readable source tag for rapid local rebuild/testing. The controller also hashes the
// actual EXE and DLL on disk, so a stale or mismatched binary is obvious in copied logs.
constexpr wchar_t kBuildTag[] = L"2026-10-05.11-sl1-worker";

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
    ReflexBackendLegacyPluginConstants = 3
};

struct ReflexEvent {
    volatile LONG sequence;
    LONGLONG qpc;
    LONG mode;
    uint32_t requestedUs;
    uint32_t effectiveUs;
    LONG result;
};

struct SharedState {
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    DWORD targetProcessId;

    volatile LONG configSequence;
    volatile LONG overrideEnabled;
    volatile LONG overrideUs;

    volatile LONG hookState;
    volatile LONG backend;
    volatile LONG eventSerial;

    wchar_t targetPath[kPathChars];
    wchar_t reflexPath[kPathChars];
    wchar_t reflexVersion[kVersionChars];
    wchar_t injectedBuild[kVersionChars];
    wchar_t lastError[kErrorChars];

    ReflexEvent events[kEventCapacity];
};

} // namespace ReflexProbeProtocol
