#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "../Common/Protocol.h"

namespace ReflexProbeController {

constexpr UINT_PTR kPollTimer = 1;
constexpr UINT_PTR kWatchTimer = 2;
constexpr UINT kWatchPollIntervalMs = 10;
constexpr int kMargin = 12;
constexpr int kStatusTop = 376;
constexpr int kMinimumWindowHeight = 615;
constexpr WPARAM kStatusTextLimit = 16u * 1024u * 1024u;
constexpr size_t kInitialTextBufferCapacity = 8 * 1024;
constexpr size_t kRawDebugCapacity = 131072;
constexpr UINT kPollIntervalMs = 50;

enum ControlId : int {
    IDC_GAME_PATH = 1001,
    IDC_BROWSE,
    IDC_ARGUMENTS,
    IDC_OVERRIDE_ENABLE,
    IDC_OVERRIDE_FPS,
    IDC_LAUNCH,
    IDC_STATUS,
    IDC_WORD_WRAP,
    IDC_CAPTURE_STATE,
    IDC_CAPTURE_RAW,
    IDC_WATCH,
    IDC_ATTACH,
    IDC_FORCE_BOOST,
    IDC_COUNT_REFLEX_SLEEP,
    IDC_CLEAR_HISTORY,
    IDC_REFLEX_PROBING,
    IDC_FG_PROBING,
    IDC_FG_FORCE_AUTO_ON,
    IDC_FG_MENU_OVERRIDE,
    IDC_FG_MENU_OFF,
    IDC_FG_MENU_ON,
    IDC_FG_RETENTION_OVERRIDE,
    IDC_FG_RETENTION_OFF,
    IDC_FG_RETENTION_ON,
    IDC_FG_WRAP_GET_STATE
};

struct FgPolicy {
    bool forceOnWhenAuto = false;
    bool overrideMenu = false;
    bool menuDetectionOn = false;
    bool overrideRetention = false;
    bool retainResourcesWhenOff = true; // when checked, default radio is On
    bool wrapGetState = false;
};

enum CaptureMode : uint32_t {
    CaptureModeStateChanges = 0,
    CaptureModeRawDebug = 1
};

struct CapturedReflexEvent {
    LONGLONG qpc;
    LONG sequence;
    LONG kind;
    LONG requestedMode;
    LONG effectiveMode;
    LONG result;
    LONG backend;
    uint32_t requestedUs;
    uint32_t effectiveUs;
    ReflexProbeProtocol::FgEventData fg{};
};

struct FgStateRun {
    bool used = false;
    LONG kind = 0;
    uint32_t viewport = 0;
    CapturedReflexEvent previous{};
    uint64_t repeats = 0;
};

constexpr size_t kFgTrackedStreams = 32;

struct TextBuffer {
    wchar_t* data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;

    HWND gameLabel = nullptr;
    HWND gamePath = nullptr;
    WNDPROC gamePathOriginalProc = nullptr;
    HWND browse = nullptr;
    HWND argumentsLabel = nullptr;
    HWND arguments = nullptr;
    HWND reflexGroup = nullptr;
    HWND reflexProbing = nullptr;
    HWND fgGroup = nullptr;
    HWND fgProbing = nullptr;
    HWND fgMultiplierOverride = nullptr;
    HWND fgMultiplierChoice = nullptr;
    HWND fgMenuOverride = nullptr;
    HWND fgMenuOff = nullptr;
    HWND fgMenuOn = nullptr;
    HWND fgRetentionOverride = nullptr;
    HWND fgRetentionOff = nullptr;
    HWND fgRetentionOn = nullptr;
    HWND fgForceAutoOn = nullptr;
    HWND fgGetState = nullptr;
    HWND overrideEnable = nullptr;
    HWND overrideFps = nullptr;
    HWND fpsLabel = nullptr;
    HWND forceBoost = nullptr;
    HWND countReflexSleep = nullptr;
    HWND launch = nullptr;
    HWND watch = nullptr;
    HWND attach = nullptr;
    HWND currentState = nullptr;
    HWND fgState = nullptr;
    HWND statusLabel = nullptr;
    HWND captureModeLabel = nullptr;
    HWND captureState = nullptr;
    HWND captureRaw = nullptr;
    HWND clearHistory = nullptr;
    HWND wordWrap = nullptr;
    HWND status = nullptr;
    WNDPROC statusOriginalProc = nullptr;

    HANDLE process = nullptr;
    DWORD processId = 0;
    HANDLE mapping = nullptr;
    ReflexProbeProtocol::SharedState* shared = nullptr;

    LONG lastHookState = -1;
    LONG lastFgHookBits = 0;
    LONG lastEventSerial = 0;
    bool loggedInjectedBuild = false;
    bool rawUiBatchWarningShown = false;

    bool haveEffectiveState = false;
    LONG currentEffectiveMode = 0;
    uint32_t currentEffectiveUs = 0;

    // Cache only the configuration fields displayed by the FG status label.
    // High-frequency identical SetOptions submissions require no Win32 text I/O.
    bool haveFgLabelState = false;
    uint32_t fgLabelMode = 0;
    uint32_t fgLabelGeneratedFrames = 0;
    bool fgLabelMenuDetection = false;

    bool watchArmed = false;
    wchar_t watchTargetPath[ReflexProbeProtocol::kPathChars]{};
    bool watchOverrideEnabled = false;
    uint32_t watchOverrideUs = 0;
    bool watchForceBoostWhenOn = false;
    bool watchCountReflexSleepCalls = false;
    bool watchProbeReflex = true;
    bool watchProbeDlssFg = false;
    FgPolicy watchFgPolicy{};
    LONGLONG watchStartQpc = 0;

    CaptureMode captureMode = CaptureModeStateChanges;
    CapturedReflexEvent* rawDebugEvents = nullptr;
    size_t rawDebugCount = 0;
    size_t rawDebugWriteIndex = 0;
    uint64_t rawDebugTotalCaptured = 0;
    bool rawDebugWrapped = false;
    bool rawRingWrapNoticePending = false;

    // RAM-only durable diagnostics, separate from disposable capture events.
    TextBuffer startupDiagnostics{};
    TextBuffer sessionDiagnostics{};
    bool capturingStartupDiagnostics = false;

    bool haveStateEvent = false;
    CapturedReflexEvent stateEvent{};
    uint64_t stateRepeatCount = 0;
    uint64_t stateSleepCount = 0;
    FgStateRun fgRuns[kFgTrackedStreams]{};
    bool fgTrackingOverflowWarned = false;
};

extern AppState g_app;

void LayoutControls(int clientWidth, int clientHeight);
void CleanupTarget();
void UpdateAcquisitionControls();

void SetChildFont(HWND child, HFONT font);
void AppendStatus(const wchar_t* text);
bool AppendTextBufferLineAtQpc(TextBuffer& buffer, const wchar_t* text, LONGLONG eventQpc);
void FreeTextBuffer(TextBuffer& buffer);
void AppendDisplayLineAtQpc(const wchar_t* text, LONGLONG eventQpc);
void AppendStatusLineAtQpc(const wchar_t* text, LONGLONG eventQpc);
void AppendStatusLine(const wchar_t* text);
void BeginNewDiagnosticSession();
void RecreateStatusControl(bool wordWrap);

const wchar_t* PathFileName(const wchar_t* path);
void PathDirectory(const wchar_t* path, wchar_t* out, size_t outCount);
bool GetSiblingDllPath(wchar_t* out, size_t outCount);
void LogBinaryIdentity();
bool GetLocalFunctionOwner(void* function, HMODULE& owner, wchar_t* moduleName, size_t moduleNameCount);
bool WaitForRemoteFunctionOwner(DWORD processId, const wchar_t* moduleName, uintptr_t& baseOut, DWORD timeoutMs);
bool InjectDll(HANDLE process, DWORD processId, const wchar_t* dllPath,
               bool allowPostLoadVerificationRecovery,
               wchar_t* error, size_t errorCount);
bool CreateSharedState(DWORD processId, const wchar_t* targetPath,
                       bool overrideEnabled, uint32_t overrideUs, bool forceBoostWhenOn,
                       bool countReflexSleepCalls, bool probeReflex, bool probeDlssFg,
                       const FgPolicy& fgPolicy, wchar_t* error, size_t errorCount);
bool ArmProcessWatch(const wchar_t* targetPath, bool overrideEnabled, uint32_t overrideUs,
                     bool forceBoostWhenOn, bool countReflexSleepCalls,
                     bool probeReflex, bool probeDlssFg, const FgPolicy& fgPolicy,
                     wchar_t* error, size_t errorCount);
void CancelProcessWatch(bool logCancellation);
void PollProcessWatch();
bool AttachRunningProcess(const wchar_t* targetPath, bool overrideEnabled, uint32_t overrideUs,
                          bool forceBoostWhenOn, bool countReflexSleepCalls,
                          bool probeReflex, bool probeDlssFg, const FgPolicy& fgPolicy,
                     wchar_t* error, size_t errorCount);

void ResetLiveStateTracking();
bool EnsureRawDebugBuffer();
void ResetRawDebugCapture();
void FreeRawDebugBuffer();
void ClearCaptureHistory();
void SetCaptureButtons(CaptureMode mode);
bool SetCaptureMode(CaptureMode mode);
void ResetCurrentEffectiveState();
void PollSharedState();

} // namespace ReflexProbeController
