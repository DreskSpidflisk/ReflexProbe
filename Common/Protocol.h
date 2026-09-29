#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

namespace ReflexProbeProtocol {

constexpr uint32_t kMagic = 0x31505246; // "FRP1" little-endian.
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEventCapacity = 128;
constexpr uint32_t kPathChars = 1024;
constexpr uint32_t kVersionChars = 64;
constexpr uint32_t kErrorChars = 512;

constexpr wchar_t kMappingPrefix[] = L"Local\\ReflexProbe.";

enum HookState : LONG {
    HookStateWaitingForDll = 0,
    HookStateReflexFound = 1,
    HookStateHooked = 2,
    HookStateError = 3
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
    volatile LONG eventSerial;

    wchar_t targetPath[kPathChars];
    wchar_t reflexPath[kPathChars];
    wchar_t reflexVersion[kVersionChars];
    wchar_t lastError[kErrorChars];

    ReflexEvent events[kEventCapacity];
};

} // namespace ReflexProbeProtocol
