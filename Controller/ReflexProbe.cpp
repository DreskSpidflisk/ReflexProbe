#include "ControllerInternal.h"

#include <windowsx.h>
#include <commdlg.h>
#include <wchar.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#pragma comment(lib, "comdlg32.lib")

namespace ReflexProbeController {

constexpr wchar_t kWindowClass[] = L"ReflexProbeControlWindow";

AppState g_app;

constexpr int kBrowseWidth = 96;
constexpr int kLaunchWidth = 108;
constexpr int kWatchWidth = 116;
constexpr int kAttachWidth = 92;
constexpr int kAcquisitionGap = 8;
constexpr int kPolicyGap = 10;
constexpr int kOverrideWidth = 172;
constexpr int kOverrideFpsWidth = 70;
constexpr int kFpsLabelWidth = 34;
constexpr int kForceBoostWidth = 171;
constexpr int kCountSleepWidth = 190;
constexpr int kWordWrapWidth = 104;
constexpr int kClearWidth = 58;
constexpr int kRawWidth = 88;
constexpr int kStateWidth = 108;
constexpr int kCaptureLabelWidth = 92;
constexpr int kCaptureGap = 8;

constexpr int kFpsX = kMargin + 18 + kOverrideWidth;
constexpr int kFpsLabelX = kFpsX + kOverrideFpsWidth + 8;
constexpr int kForceBoostX = kFpsLabelX + kFpsLabelWidth + kPolicyGap;
constexpr int kCountSleepX = kForceBoostX + kForceBoostWidth + kPolicyGap;
constexpr int kPolicyRight = kCountSleepX + kCountSleepWidth;
constexpr int kMinimumClientWidth = kPolicyRight + kMargin + 36;

constexpr int kReflexGroupTop = 127;
constexpr int kReflexGroupHeight = 56;
constexpr int kFgGroupTop = 198;
constexpr int kFgGroupHeight = 92;
constexpr int kAcquisitionTop = 303;
constexpr int kReflexStateTop = kAcquisitionTop - 1;
constexpr int kFgStateTop = kAcquisitionTop + 15;
constexpr int kStatusHeadingTop = 346;
constexpr int kAcquisitionStatusGap = 8;

bool IsSelectedGameExecutableValid()
{
    if (!g_app.gamePath)
        return false;

    wchar_t gamePath[ReflexProbeProtocol::kPathChars]{};
    GetWindowTextW(g_app.gamePath, gamePath, static_cast<int>(_countof(gamePath)));
    if (!gamePath[0])
        return false;

    const DWORD attributes = GetFileAttributesW(gamePath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int MinimumWindowTrackWidth(HWND window)
{
    RECT frame{ 0, 0, kMinimumClientWidth, 1 };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if (AdjustWindowRectEx(&frame, style, FALSE, exStyle))
        return frame.right - frame.left;
    return kMinimumClientWidth;
}

LRESULT CALLBACK GamePathEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        UpdateAcquisitionControls();
        return 0;
    }
    if (message == WM_CHAR && wParam == VK_RETURN)
        return 0;

    if (g_app.gamePathOriginalProc)
        return CallWindowProcW(g_app.gamePathOriginalProc, window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ParseOverrideFromUi(bool& enabled, uint32_t& frameLimitUs, wchar_t* error, size_t errorCount)
{
    enabled = Button_GetCheck(g_app.overrideEnable) == BST_CHECKED;
    frameLimitUs = 0;

    if (!enabled)
        return true;

    wchar_t fpsText[64]{};
    GetWindowTextW(g_app.overrideFps, fpsText, static_cast<int>(_countof(fpsText)));

    wchar_t* end = nullptr;
    const double fps = wcstod(fpsText, &end);
    if (end == fpsText || !_finite(fps) || fps < 0.0) {
        swprintf_s(error, errorCount, L"Override FPS must be zero or a positive number.");
        return false;
    }

    if (fps == 0.0) {
        frameLimitUs = 0;
        return true;
    }

    double interval = 1000000.0 / fps;
    if (interval < 1.0)
        interval = 1.0;
    if (interval > 4294967295.0)
        interval = 4294967295.0;

    frameLimitUs = static_cast<uint32_t>(interval + 0.5);
    return true;
}

bool ReflexProbingSelected()
{
    return g_app.reflexProbing && Button_GetCheck(g_app.reflexProbing) == BST_CHECKED;
}

bool FgProbingSelected()
{
    return g_app.fgProbing && Button_GetCheck(g_app.fgProbing) == BST_CHECKED;
}

FgPolicy ReadFgPolicyFromUi()
{
    FgPolicy policy{};
    if (FgProbingSelected()) {
        policy.forceOnWhenAuto = Button_GetCheck(g_app.fgForceAutoOn) == BST_CHECKED;
        policy.overrideMenu = Button_GetCheck(g_app.fgMenuOverride) == BST_CHECKED;
        policy.menuDetectionOn = Button_GetCheck(g_app.fgMenuOn) == BST_CHECKED;
        policy.overrideRetention = Button_GetCheck(g_app.fgRetentionOverride) == BST_CHECKED;
        policy.retainResourcesWhenOff = Button_GetCheck(g_app.fgRetentionOn) == BST_CHECKED;
        policy.wrapGetState = Button_GetCheck(g_app.fgGetState) == BST_CHECKED;
    }
    return policy;
}

void ApplyFgPolicyToShared()
{
    if (!g_app.shared || !FgProbingSelected())
        return;
    const FgPolicy policy = ReadFgPolicyFromUi();
    InterlockedExchange(&g_app.shared->fgForceOnWhenAuto, policy.forceOnWhenAuto ? 1 : 0);
    InterlockedExchange(&g_app.shared->fgMenuOverrideEnabled, policy.overrideMenu ? 1 : 0);
    InterlockedExchange(&g_app.shared->fgMenuOverrideOn, policy.menuDetectionOn ? 1 : 0);
    InterlockedExchange(&g_app.shared->fgRetentionOverrideEnabled,
        policy.overrideRetention ? 1 : 0);
    InterlockedExchange(&g_app.shared->fgRetentionOverrideOn,
        policy.retainResourcesWhenOff ? 1 : 0);
    InterlockedIncrement(&g_app.shared->configSequence);
    AppendStatusLine(policy.forceOnWhenAuto
        ? L"FG policy: Force Auto to On enabled; applies to next SetOptions call."
        : L"FG policy: Force Auto to On disabled.");
    AppendStatusLine(!policy.overrideMenu
        ? L"FG policy: fullscreen-menu-detection override disabled."
        : (policy.menuDetectionOn
            ? L"FG policy: fullscreen-menu-detection override forced On."
            : L"FG policy: fullscreen-menu-detection override forced Off."));
    AppendStatusLine(!policy.overrideRetention
        ? L"FG policy: resource-retention override disabled."
        : (policy.retainResourcesWhenOff
            ? L"FG policy: resource-retention override forced On (retained VRAM possible while Off)."
            : L"FG policy: resource-retention override forced Off."));
}

void ApplyPolicyToShared(bool logFrameLimit, bool logBoost)
{
    if (!g_app.shared)
        return;

    bool enabled = false;
    uint32_t frameLimitUs = 0;
    wchar_t error[256]{};
    if (!ParseOverrideFromUi(enabled, frameLimitUs, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return;
    }

    const bool forceBoostWhenOn = Button_GetCheck(g_app.forceBoost) == BST_CHECKED;

    InterlockedExchange(&g_app.shared->overrideUs, static_cast<LONG>(frameLimitUs));
    InterlockedExchange(&g_app.shared->overrideEnabled, enabled ? 1 : 0);
    InterlockedExchange(&g_app.shared->forceBoostWhenOn, forceBoostWhenOn ? 1 : 0);
    InterlockedIncrement(&g_app.shared->configSequence);

    wchar_t line[320]{};
    if (logFrameLimit) {
        if (enabled && frameLimitUs)
            swprintf_s(line, L"Frame-limit override armed: %u us (%.3f FPS). Takes effect on the next Reflex settings call.",
                frameLimitUs, 1000000.0 / static_cast<double>(frameLimitUs));
        else if (enabled)
            wcscpy_s(line, L"Frame-limit override armed: 0 us (no explicit Reflex frame limit). Takes effect on the next Reflex settings call.");
        else
            wcscpy_s(line, L"Frame-limit override disabled. Game frameLimitUs requests will pass through unchanged.");
        AppendStatusLine(line);
    }

    if (logBoost) {
        if (forceBoostWhenOn)
            wcscpy_s(line, L"Force Boost when Reflex On enabled. Plain On requests will be forwarded as On + Boost on the next Reflex settings call.");
        else
            wcscpy_s(line, L"Force Boost when Reflex On disabled. Reflex mode requests will pass through unchanged.");
        AppendStatusLine(line);
    }
}
void UpdateAcquisitionControls()
{
    const bool targetActive = g_app.process != nullptr;
    const bool watching = g_app.watchArmed;
    const bool validTarget = IsSelectedGameExecutableValid();
    const bool reflexEnabled = ReflexProbingSelected();
    const bool fgEnabled = FgProbingSelected();
    const bool anyProbe = reflexEnabled || fgEnabled;
    const bool probeSelectionLocked = targetActive || watching;

    if (g_app.launch)
        EnableWindow(g_app.launch, validTarget && anyProbe && !targetActive && !watching);
    if (g_app.attach)
        EnableWindow(g_app.attach, validTarget && anyProbe && !targetActive && !watching);
    if (g_app.watch) {
        EnableWindow(g_app.watch, watching || (validTarget && anyProbe && !targetActive));
        SetWindowTextW(g_app.watch, watching ? L"Cancel Watch" : L"Watch + Inject");
    }

    if (g_app.gamePath)
        EnableWindow(g_app.gamePath, !watching);
    if (g_app.browse)
        EnableWindow(g_app.browse, !watching);
    if (g_app.arguments)
        EnableWindow(g_app.arguments, !watching);

    if (g_app.reflexProbing)
        EnableWindow(g_app.reflexProbing, !probeSelectionLocked);
    if (g_app.fgProbing)
        EnableWindow(g_app.fgProbing, !probeSelectionLocked);
    if (g_app.fgForceAutoOn)
        EnableWindow(g_app.fgForceAutoOn, fgEnabled && !watching);
    if (g_app.fgMenuOverride)
        EnableWindow(g_app.fgMenuOverride, fgEnabled && !watching);
    const bool menuOverrideChecked = g_app.fgMenuOverride &&
        Button_GetCheck(g_app.fgMenuOverride) == BST_CHECKED;
    if (g_app.fgMenuOff)
        EnableWindow(g_app.fgMenuOff, fgEnabled && !watching && menuOverrideChecked);
    if (g_app.fgMenuOn)
        EnableWindow(g_app.fgMenuOn, fgEnabled && !watching && menuOverrideChecked);
    if (g_app.fgRetentionOverride)
        EnableWindow(g_app.fgRetentionOverride, fgEnabled && !watching);
    const bool retentionChecked = g_app.fgRetentionOverride &&
        Button_GetCheck(g_app.fgRetentionOverride) == BST_CHECKED;
    if (g_app.fgRetentionOff)
        EnableWindow(g_app.fgRetentionOff, fgEnabled && !watching && retentionChecked);
    if (g_app.fgRetentionOn)
        EnableWindow(g_app.fgRetentionOn, fgEnabled && !watching && retentionChecked);
    if (g_app.fgGetState)
        EnableWindow(g_app.fgGetState, fgEnabled && !probeSelectionLocked);

    if (g_app.overrideEnable)
        EnableWindow(g_app.overrideEnable, reflexEnabled && !watching);
    if (g_app.overrideFps) {
        const bool frameOverrideChecked =
            g_app.overrideEnable && Button_GetCheck(g_app.overrideEnable) == BST_CHECKED;
        EnableWindow(g_app.overrideFps, reflexEnabled && !watching && frameOverrideChecked);
    }
    if (g_app.forceBoost)
        EnableWindow(g_app.forceBoost, reflexEnabled && !watching);
    if (g_app.countReflexSleep)
        EnableWindow(g_app.countReflexSleep, reflexEnabled && !targetActive && !watching);
    if (g_app.clearHistory)
        EnableWindow(g_app.clearHistory, !targetActive && !watching);
}

void CleanupTarget()
{
    if (g_app.watchArmed)
        CancelProcessWatch(false);

    if (g_app.shared) {
        UnmapViewOfFile(g_app.shared);
        g_app.shared = nullptr;
    }
    if (g_app.mapping) {
        CloseHandle(g_app.mapping);
        g_app.mapping = nullptr;
    }
    if (g_app.process) {
        CloseHandle(g_app.process);
        g_app.process = nullptr;
    }

    g_app.processId = 0;
    g_app.lastHookState = -1;
    g_app.lastFgHookBits = 0;
    g_app.lastEventSerial = 0;
    g_app.loggedInjectedBuild = false;
    g_app.rawUiBatchWarningShown = false;
    ResetLiveStateTracking();
    ResetCurrentEffectiveState();
    UpdateAcquisitionControls();
}

bool LaunchAndInject()
{
    if (g_app.process || g_app.watchArmed)
        return false;

    wchar_t gamePath[ReflexProbeProtocol::kPathChars]{};
    wchar_t arguments[4096]{};
    GetWindowTextW(g_app.gamePath, gamePath, static_cast<int>(_countof(gamePath)));
    GetWindowTextW(g_app.arguments, arguments, static_cast<int>(_countof(arguments)));

    if (!gamePath[0]) {
        MessageBoxW(g_app.window, L"Choose a game executable first.", L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    DWORD attributes = GetFileAttributesW(gamePath);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_app.window, L"The selected game executable does not exist.", L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    bool overrideEnabled = false;
    uint32_t overrideUs = 0;
    const bool forceBoostWhenOn = Button_GetCheck(g_app.forceBoost) == BST_CHECKED;
    const bool countReflexSleepCalls =
        Button_GetCheck(g_app.countReflexSleep) == BST_CHECKED;
    const bool probeReflex = ReflexProbingSelected();
    const bool probeDlssFg = FgProbingSelected();
    const FgPolicy fgPolicy = ReadFgPolicyFromUi();
    if (!probeReflex && !probeDlssFg)
        return false;
    wchar_t error[512]{};
    if (probeReflex && !ParseOverrideFromUi(overrideEnabled, overrideUs, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    if (g_app.captureMode == CaptureModeRawDebug) {
        if (!EnsureRawDebugBuffer()) {
            MessageBoxW(g_app.window,
                L"Could not allocate the bounded Raw debug capture buffer.",
                L"ReflexProbe", MB_ICONERROR);
            return false;
        }
        ResetRawDebugCapture();
    }
    ResetLiveStateTracking();

    wchar_t dllPath[MAX_PATH]{};
    if (!GetSiblingDllPath(dllPath, _countof(dllPath)) || GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_app.window,
            L"ReflexProbe64.dll was not found beside ReflexProbe.exe. Build both projects in the solution.",
            L"ReflexProbe", MB_ICONERROR);
        return false;
    }

    wchar_t workingDirectory[ReflexProbeProtocol::kPathChars]{};
    PathDirectory(gamePath, workingDirectory, _countof(workingDirectory));

    wchar_t commandLine[32768]{};
    if (arguments[0])
        swprintf_s(commandLine, L"\"%s\" %s", gamePath, arguments);
    else
        swprintf_s(commandLine, L"\"%s\"", gamePath);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(gamePath, commandLine, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, workingDirectory[0] ? workingDirectory : nullptr, &startup, &process)) {
        swprintf_s(error, L"CreateProcess failed (%lu).", GetLastError());
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONERROR);
        return false;
    }

    g_app.process = process.hProcess;
    g_app.processId = process.dwProcessId;

    BOOL targetIsWow64 = FALSE;
    if (IsWow64Process(process.hProcess, &targetIsWow64) && targetIsWow64) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        MessageBoxW(g_app.window, L"ReflexProbe bootstrap supports x64 targets only.", L"ReflexProbe", MB_ICONERROR);
        CleanupTarget();
        return false;
    }

    if (!CreateSharedState(process.dwProcessId, gamePath,
            overrideEnabled, overrideUs, forceBoostWhenOn, countReflexSleepCalls,
            probeReflex, probeDlssFg, fgPolicy, error, _countof(error))) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONERROR);
        CleanupTarget();
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr;
    HMODULE owner = nullptr;
    wchar_t ownerName[MAX_PATH]{};
    uintptr_t remoteOwner = 0;
    bool resumed = false;

    if (!loadLibrary || !GetLocalFunctionOwner(reinterpret_cast<void*>(loadLibrary), owner, ownerName, _countof(ownerName)) ||
        !WaitForRemoteFunctionOwner(process.dwProcessId, ownerName, remoteOwner, 50)) {
        ResumeThread(process.hThread);
        resumed = true;
    }

    if (!InjectDll(process.hProcess, process.dwProcessId, dllPath, true, error, _countof(error))) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        MessageBoxW(g_app.window, error, L"ReflexProbe injection failed", MB_ICONERROR);
        CleanupTarget();
        return false;
    }

    if (!resumed)
        ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    g_app.lastHookState = -1;
    g_app.lastFgHookBits = 0;
    g_app.lastEventSerial = 0;
    g_app.loggedInjectedBuild = false;
    g_app.rawUiBatchWarningShown = false;
    UpdateAcquisitionControls();

    wchar_t line[512]{};
    swprintf_s(line, L"Launched %s (PID %lu) and injected ReflexProbe64.dll.", PathFileName(gamePath), process.dwProcessId);
    AppendStatusLine(line);
    AppendStatusLine(probeReflex
        ? L"Reflex probing: enabled; supported Reflex functions can be wrapped."
        : L"Reflex probing: disabled; Reflex functions are passed through untouched.");
    AppendStatusLine(probeDlssFg
        ? (fgPolicy.wrapGetState
            ? L"DLSS FG probing: SetOptions and GetState intercepted."
            : L"DLSS FG probing: SetOptions intercepted; GetState pointer untouched.")
        : L"DLSS FG probing: disabled; FG functions are passed through untouched.");
    if (probeDlssFg) {
        AppendStatusLine(fgPolicy.forceOnWhenAuto
            ? L"Initial FG Auto-to-On override: enabled."
            : L"Initial FG Auto-to-On override: disabled.");
        AppendStatusLine(!fgPolicy.overrideMenu
            ? L"Initial FG menu-detection override: disabled."
            : (fgPolicy.menuDetectionOn
                ? L"Initial FG menu-detection override: On."
                : L"Initial FG menu-detection override: Off."));
        AppendStatusLine(!fgPolicy.overrideRetention
            ? L"Initial FG resource-retention override: disabled."
            : (fgPolicy.retainResourcesWhenOff
                ? L"Initial FG resource-retention override: On (VRAM retained while FG Off)."
                : L"Initial FG resource-retention override: Off."));
    }
    if (probeReflex && overrideEnabled && overrideUs) {
        swprintf_s(line, L"Initial override: %u us (%.3f FPS).", overrideUs,
            1000000.0 / static_cast<double>(overrideUs));
        AppendStatusLine(line);
    } else if (probeReflex && overrideEnabled) {
        AppendStatusLine(L"Initial override: 0 us (no explicit Reflex frame limit).");
    } else if (probeReflex) {
        AppendStatusLine(L"Initial frame-limit override: disabled.");
    }

    if (probeReflex && forceBoostWhenOn)
        AppendStatusLine(L"Initial Force Boost policy: enabled; plain Reflex On requests will be forwarded as On + Boost.");
    else if (probeReflex)
        AppendStatusLine(L"Initial Force Boost policy: disabled; Reflex mode requests pass through unchanged.");

    if (probeReflex)
        AppendStatusLine(countReflexSleepCalls
            ? L"Initial Reflex Sleep call counting: enabled; Reflex Sleep will be wrapped for this target."
            : L"Initial Reflex Sleep call counting: disabled; Reflex Sleep will remain untouched for this target.");

    if (g_app.captureMode == CaptureModeRawDebug)
        AppendStatusLine(L"Capture mode: Raw debug; bounded raw retention starts with this target.");
    else
        AppendStatusLine(L"Capture mode: State changes; identical Reflex and per-viewport FG calls are counted.");

    return true;
}

bool ReadTargetAndPolicyForExternalAcquisition(
    wchar_t* gamePath, size_t gamePathCount,
    bool& overrideEnabled, uint32_t& overrideUs, bool& forceBoostWhenOn,
    bool& countReflexSleepCalls, bool& probeReflex, bool& probeDlssFg,
    FgPolicy& fgPolicy, wchar_t* error, size_t errorCount)
{
    if (!gamePath || !gamePathCount)
        return false;

    GetWindowTextW(g_app.gamePath, gamePath, static_cast<int>(gamePathCount));
    if (!gamePath[0]) {
        swprintf_s(error, errorCount, L"Choose a game executable first.");
        return false;
    }

    const DWORD attributes = GetFileAttributesW(gamePath);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        swprintf_s(error, errorCount, L"The selected game executable does not exist.");
        return false;
    }

    probeReflex = ReflexProbingSelected();
    probeDlssFg = FgProbingSelected();
    fgPolicy = ReadFgPolicyFromUi();
    if (!probeReflex && !probeDlssFg) {
        swprintf_s(error, errorCount, L"Enable at least one probe before acquisition.");
        return false;
    }
    forceBoostWhenOn = Button_GetCheck(g_app.forceBoost) == BST_CHECKED;
    countReflexSleepCalls =
        Button_GetCheck(g_app.countReflexSleep) == BST_CHECKED;
    if (!probeReflex) {
        overrideEnabled = false;
        overrideUs = 0;
        forceBoostWhenOn = false;
        countReflexSleepCalls = false;
        return true;
    }
    return ParseOverrideFromUi(overrideEnabled, overrideUs, error, errorCount);
}

bool ToggleWatchAndInject()
{
    if (g_app.watchArmed) {
        CancelProcessWatch(true);
        return true;
    }

    wchar_t gamePath[ReflexProbeProtocol::kPathChars]{};
    bool overrideEnabled = false;
    uint32_t overrideUs = 0;
    bool forceBoostWhenOn = false;
    bool countReflexSleepCalls = false;
    bool probeReflex = true;
    bool probeDlssFg = false;
    FgPolicy fgPolicy{};
    wchar_t error[512]{};

    if (!ReadTargetAndPolicyForExternalAcquisition(
            gamePath, _countof(gamePath), overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    if (!ArmProcessWatch(gamePath, overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe Watch + Inject", MB_ICONERROR);
        return false;
    }

    return true;
}

bool AttachToRunningTarget()
{
    wchar_t gamePath[ReflexProbeProtocol::kPathChars]{};
    bool overrideEnabled = false;
    uint32_t overrideUs = 0;
    bool forceBoostWhenOn = false;
    bool countReflexSleepCalls = false;
    bool probeReflex = true;
    bool probeDlssFg = false;
    FgPolicy fgPolicy{};
    wchar_t error[512]{};

    if (!ReadTargetAndPolicyForExternalAcquisition(
            gamePath, _countof(gamePath), overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    if (!AttachRunningProcess(gamePath, overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, probeReflex, probeDlssFg, fgPolicy, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe Attach", MB_ICONERROR);
        return false;
    }

    return true;
}

void BrowseForGame()
{
    wchar_t path[ReflexProbeProtocol::kPathChars]{};
    GetWindowTextW(g_app.gamePath, path, static_cast<int>(_countof(path)));

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app.window;
    dialog.lpstrFilter = L"Windows executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(_countof(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&dialog)) {
        SetWindowTextW(g_app.gamePath, path);
        UpdateAcquisitionControls();
    }
}

// A checkbox painted over a group-box caption interrupts its border for the entire
// checkbox rectangle. Size the caption to its actual font/text instead of leaving
// a long blank section of border hidden by a fixed-width checkbox.
int MeasureGroupHeadingWidth(HWND checkbox)
{
    if (!checkbox)
        return 0;

    wchar_t label[128]{};
    GetWindowTextW(checkbox, label, static_cast<int>(_countof(label)));

    HDC dc = GetDC(checkbox);
    if (!dc)
        return 0;

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(checkbox, WM_GETFONT, 0, 0));
    HGDIOBJ previous = nullptr;
    if (font)
        previous = SelectObject(dc, font);

    SIZE extent{};
    const BOOL measured = GetTextExtentPoint32W(
        dc, label, static_cast<int>(wcslen(label)), &extent);

    if (previous)
        SelectObject(dc, previous);
    ReleaseDC(checkbox, dc);

    if (!measured)
        return 0;

    // Checkbox glyph + text gap + a little trailing breathing room.
    return GetSystemMetrics(SM_CXMENUCHECK) + extent.cx + 12;
}

// The radio glyph is already accounted for by MeasureGroupHeadingWidth.
// Its heading-specific extra trailing gap is unnecessary in a packed row.
int MeasureCompactRadioWidth(HWND radio)
{
    const int width = MeasureGroupHeadingWidth(radio);
    return width > 8 ? width - 6 : 40;
}

void LayoutControls(int clientWidth, int clientHeight)
{
    if (clientWidth <= 0 || clientHeight <= 0)
        return;

    const int usableWidth = clientWidth - (kMargin * 2);

    if (g_app.gameLabel)
        MoveWindow(g_app.gameLabel, kMargin, 12, 150, 20, TRUE);
    if (g_app.browse)
        MoveWindow(g_app.browse, clientWidth - kMargin - kBrowseWidth, 33, kBrowseWidth, 26, TRUE);
    if (g_app.gamePath) {
        const int gameWidth = usableWidth - kBrowseWidth - 10;
        MoveWindow(g_app.gamePath, kMargin, 34, gameWidth > 50 ? gameWidth : 50, 24, TRUE);
    }

    if (g_app.argumentsLabel)
        MoveWindow(g_app.argumentsLabel, kMargin, 68, 150, 20, TRUE);
    if (g_app.arguments)
        MoveWindow(g_app.arguments, kMargin, 90, usableWidth > 50 ? usableWidth : 50, 24, TRUE);

    if (g_app.reflexGroup)
        MoveWindow(g_app.reflexGroup, kMargin, kReflexGroupTop,
            usableWidth, kReflexGroupHeight, TRUE);
    if (g_app.reflexProbing)
        MoveWindow(g_app.reflexProbing, kMargin + 14, kReflexGroupTop - 9,
            MeasureGroupHeadingWidth(g_app.reflexProbing), 22, TRUE);

    if (g_app.overrideEnable)
        MoveWindow(g_app.overrideEnable, kMargin + 18, 149, kOverrideWidth, 22, TRUE);
    if (g_app.overrideFps)
        MoveWindow(g_app.overrideFps, kFpsX, 148, kOverrideFpsWidth, 24, TRUE);
    if (g_app.fpsLabel)
        MoveWindow(g_app.fpsLabel, kFpsLabelX, 151, kFpsLabelWidth, 20, TRUE);
    if (g_app.forceBoost)
        MoveWindow(g_app.forceBoost, kForceBoostX, 149, kForceBoostWidth, 22, TRUE);
    if (g_app.countReflexSleep)
        MoveWindow(g_app.countReflexSleep, kCountSleepX, 149, kCountSleepWidth, 22, TRUE);

    if (g_app.fgGroup)
        MoveWindow(g_app.fgGroup, kMargin, kFgGroupTop,
            usableWidth, kFgGroupHeight, TRUE);
    if (g_app.fgProbing)
        MoveWindow(g_app.fgProbing, kMargin + 14, kFgGroupTop - 9,
            MeasureGroupHeadingWidth(g_app.fgProbing), 22, TRUE);

    // Compact one-line FG override layout, including both independent Off/On
    // radio groups. Text-derived widths reclaim the blank areas of fixed slots
    // at the minimum window size while still using the actual UI font.
    int fgX = kMargin + 18;
    const int fgRowY = 220;
    const int fgControlHeight = 22;
    const int fgGroupGap = 5;
    const int fgRadioGap = 1;
    if (g_app.fgForceAutoOn) {
        const int width = MeasureGroupHeadingWidth(g_app.fgForceAutoOn);
        MoveWindow(g_app.fgForceAutoOn, fgX, fgRowY, width, fgControlHeight, TRUE);
        fgX += width + fgGroupGap;
    }
    if (g_app.fgMenuOverride) {
        const int width = MeasureGroupHeadingWidth(g_app.fgMenuOverride);
        MoveWindow(g_app.fgMenuOverride, fgX, fgRowY, width, fgControlHeight, TRUE);
        fgX += width + fgRadioGap;
    }
    if (g_app.fgMenuOff) {
        const int width = MeasureCompactRadioWidth(g_app.fgMenuOff);
        MoveWindow(g_app.fgMenuOff, fgX, fgRowY, width, fgControlHeight, TRUE);
        fgX += width;
    }
    if (g_app.fgMenuOn) {
        const int width = MeasureCompactRadioWidth(g_app.fgMenuOn);
        MoveWindow(g_app.fgMenuOn, fgX, fgRowY, width, fgControlHeight, TRUE);
        fgX += width + fgGroupGap;
    }
    if (g_app.fgRetentionOverride) {
        const int width = MeasureGroupHeadingWidth(g_app.fgRetentionOverride);
        MoveWindow(g_app.fgRetentionOverride, fgX, fgRowY, width, fgControlHeight, TRUE);
        fgX += width + fgRadioGap;
    }
    if (g_app.fgRetentionOff) {
        const int width = MeasureCompactRadioWidth(g_app.fgRetentionOff);
        MoveWindow(g_app.fgRetentionOff, fgX, fgRowY, width, fgControlHeight, TRUE);
        fgX += width;
    }
    if (g_app.fgRetentionOn) {
        const int width = MeasureCompactRadioWidth(g_app.fgRetentionOn);
        MoveWindow(g_app.fgRetentionOn, fgX, fgRowY, width, fgControlHeight, TRUE);
    }
    if (g_app.fgMultiplierOverride)
        MoveWindow(g_app.fgMultiplierOverride, kMargin + 18, 250, 168, 22, TRUE);
    if (g_app.fgMultiplierChoice)
        MoveWindow(g_app.fgMultiplierChoice, kMargin + 192, 248, 84, 112, TRUE);
    if (g_app.fgGetState)
        MoveWindow(g_app.fgGetState, kMargin + 294, 250, 250, 22, TRUE);

    const int attachX = clientWidth - kMargin - kAttachWidth;
    const int watchX = attachX - kAcquisitionGap - kWatchWidth;
    const int launchX = watchX - kAcquisitionGap - kLaunchWidth;
    if (g_app.launch)
        MoveWindow(g_app.launch, launchX, kAcquisitionTop, kLaunchWidth, 30, TRUE);
    if (g_app.watch)
        MoveWindow(g_app.watch, watchX, kAcquisitionTop, kWatchWidth, 30, TRUE);
    if (g_app.attach)
        MoveWindow(g_app.attach, attachX, kAcquisitionTop, kAttachWidth, 30, TRUE);

    // Both status lines share the acquisition-button height. The available label
    // width is bounded by Launch's left edge even at minimum window width.
    const int stateWidth = launchX - kMargin - kAcquisitionStatusGap;
    if (g_app.currentState)
        MoveWindow(g_app.currentState, kMargin, kReflexStateTop,
            stateWidth > 50 ? stateWidth : 50, 16, TRUE);
    if (g_app.fgState)
        MoveWindow(g_app.fgState, kMargin, kFgStateTop,
            stateWidth > 50 ? stateWidth : 50, 16, TRUE);

    if (g_app.statusLabel)
        MoveWindow(g_app.statusLabel, kMargin, kStatusHeadingTop, 180, 20, TRUE);

    const int wrapX = clientWidth - kMargin - kWordWrapWidth;
    const int clearX = wrapX - kCaptureGap - kClearWidth;
    const int rawX = clearX - kCaptureGap - kRawWidth;
    const int stateX = rawX - kCaptureGap - kStateWidth;
    const int captureLabelX = stateX - kCaptureGap - kCaptureLabelWidth;
    if (g_app.captureModeLabel)
        MoveWindow(g_app.captureModeLabel, captureLabelX, kStatusHeadingTop, kCaptureLabelWidth, 20, TRUE);
    if (g_app.captureState)
        MoveWindow(g_app.captureState, stateX, kStatusHeadingTop - 2, kStateWidth, 22, TRUE);
    if (g_app.captureRaw)
        MoveWindow(g_app.captureRaw, rawX, kStatusHeadingTop - 2, kRawWidth, 22, TRUE);
    if (g_app.clearHistory)
        MoveWindow(g_app.clearHistory, clearX, kStatusHeadingTop - 3, kClearWidth, 24, TRUE);
    if (g_app.wordWrap)
        MoveWindow(g_app.wordWrap, wrapX, kStatusHeadingTop - 2, kWordWrapWidth, 22, TRUE);

    if (g_app.status) {
        int statusHeight = clientHeight - kStatusTop - kMargin;
        if (statusHeight < 80)
            statusHeight = 80;
        MoveWindow(g_app.status, kMargin, kStatusTop, usableWidth > 50 ? usableWidth : 50, statusHeight, TRUE);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_app.window = window;

        g_app.gameLabel = CreateWindowExW(0, L"STATIC", L"Game executable",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.gameLabel, font);

        g_app.gamePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_GAME_PATH), g_app.instance, nullptr);
        SetChildFont(g_app.gamePath, font);
        g_app.gamePathOriginalProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_app.gamePath, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&GamePathEditProc)));

        g_app.browse = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_BROWSE), g_app.instance, nullptr);
        SetChildFont(g_app.browse, font);

        g_app.argumentsLabel = CreateWindowExW(0, L"STATIC", L"Arguments (optional)",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.argumentsLabel, font);

        g_app.arguments = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_ARGUMENTS), g_app.instance, nullptr);
        SetChildFont(g_app.arguments, font);

        // Independent acquisition-time feature switches in stacked native group boxes.
        // The checkbox sits over the upper border; the groups are created first for z-order.
        g_app.reflexGroup = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.reflexGroup, font);

        g_app.reflexProbing = CreateWindowExW(0, L"BUTTON", L"Enable Reflex probing",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_REFLEX_PROBING),
            g_app.instance, nullptr);
        SetChildFont(g_app.reflexProbing, font);
        Button_SetCheck(g_app.reflexProbing, BST_CHECKED);

        g_app.fgGroup = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.fgGroup, font);

        g_app.fgProbing = CreateWindowExW(0, L"BUTTON", L"Enable DLSS Frame Generation probing",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_PROBING),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgProbing, font);

        g_app.fgForceAutoOn = CreateWindowExW(0, L"BUTTON",
            L"Force Auto to On",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_FORCE_AUTO_ON),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgForceAutoOn, font);

        // Multiplier override is reserved for a later, capability-aware build.
        g_app.fgMultiplierOverride = CreateWindowExW(0, L"BUTTON",
            L"Override FG multiplier",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.fgMultiplierOverride, font);

        g_app.fgMultiplierChoice = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.fgMultiplierChoice, font);
        SendMessageW(g_app.fgMultiplierChoice, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"Auto"));
        SendMessageW(g_app.fgMultiplierChoice, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"2X"));
        SendMessageW(g_app.fgMultiplierChoice, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"3X"));
        SendMessageW(g_app.fgMultiplierChoice, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"4X"));
        SendMessageW(g_app.fgMultiplierChoice, CB_SETCURSEL, 0, 0);

        g_app.fgMenuOverride = CreateWindowExW(0, L"BUTTON",
            L"Override menu detection",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_MENU_OVERRIDE),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgMenuOverride, font);

        g_app.fgMenuOff = CreateWindowExW(0, L"BUTTON", L"Off",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_MENU_OFF),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgMenuOff, font);
        Button_SetCheck(g_app.fgMenuOff, BST_CHECKED);

        g_app.fgMenuOn = CreateWindowExW(0, L"BUTTON", L"On",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_MENU_ON),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgMenuOn, font);

        g_app.fgRetentionOverride = CreateWindowExW(0, L"BUTTON",
            L"Override FG resource retention",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_RETENTION_OVERRIDE),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgRetentionOverride, font);

        // Independent radio group: On is the default *selection*, but the
        // override checkbox itself starts unchecked (no intervention).
        g_app.fgRetentionOff = CreateWindowExW(0, L"BUTTON", L"Off",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_RETENTION_OFF),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgRetentionOff, font);

        g_app.fgRetentionOn = CreateWindowExW(0, L"BUTTON", L"On",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_RETENTION_ON),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgRetentionOn, font);
        Button_SetCheck(g_app.fgRetentionOn, BST_CHECKED);

        g_app.fgGetState = CreateWindowExW(0, L"BUTTON",
            L"Intercept FG GetState calls",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_GROUP,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FG_WRAP_GET_STATE),
            g_app.instance, nullptr);
        SetChildFont(g_app.fgGetState, font);

        g_app.overrideEnable = CreateWindowExW(0, L"BUTTON", L"Override Reflex frame limit",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_OVERRIDE_ENABLE), g_app.instance, nullptr);
        SetChildFont(g_app.overrideEnable, font);

        g_app.overrideFps = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"158",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_OVERRIDE_FPS), g_app.instance, nullptr);
        SetChildFont(g_app.overrideFps, font);
        EnableWindow(g_app.overrideFps, FALSE);

        g_app.fpsLabel = CreateWindowExW(0, L"STATIC", L"FPS",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.fpsLabel, font);

        g_app.forceBoost = CreateWindowExW(0, L"BUTTON", L"Force Boost when Reflex On",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_FORCE_BOOST), g_app.instance, nullptr);
        SetChildFont(g_app.forceBoost, font);

        g_app.countReflexSleep = CreateWindowExW(0, L"BUTTON", L"Count Reflex Sleep Calls",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_COUNT_REFLEX_SLEEP), g_app.instance, nullptr);
        SetChildFont(g_app.countReflexSleep, font);

        g_app.launch = CreateWindowExW(0, L"BUTTON", L"Launch + Inject",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LAUNCH), g_app.instance, nullptr);
        SetChildFont(g_app.launch, font);

        g_app.watch = CreateWindowExW(0, L"BUTTON", L"Watch + Inject",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_WATCH), g_app.instance, nullptr);
        SetChildFont(g_app.watch, font);

        g_app.attach = CreateWindowExW(0, L"BUTTON", L"Attach",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_ATTACH), g_app.instance, nullptr);
        SetChildFont(g_app.attach, font);

        g_app.currentState = CreateWindowExW(0, L"STATIC",
            L"Reflex: Unknown | Limit: Unknown",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.currentState, font);

        g_app.fgState = CreateWindowExW(0, L"STATIC",
            L"DLSS FG: Probing disabled",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.fgState, font);

        g_app.statusLabel = CreateWindowExW(0, L"STATIC", L"Status / API requests",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.statusLabel, font);

        g_app.captureModeLabel = CreateWindowExW(0, L"STATIC", L"Capture Mode:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.captureModeLabel, font);

        g_app.captureState = CreateWindowExW(0, L"BUTTON", L"State changes",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_CAPTURE_STATE), g_app.instance, nullptr);
        SetChildFont(g_app.captureState, font);

        g_app.captureRaw = CreateWindowExW(0, L"BUTTON", L"Raw debug",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_CAPTURE_RAW), g_app.instance, nullptr);
        SetChildFont(g_app.captureRaw, font);
        SetCaptureButtons(CaptureModeStateChanges);

        g_app.clearHistory = CreateWindowExW(0, L"BUTTON", L"Clear",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_CLEAR_HISTORY), g_app.instance, nullptr);
        SetChildFont(g_app.clearHistory, font);

        g_app.wordWrap = CreateWindowExW(0, L"BUTTON", L"Word wrap",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_WORD_WRAP), g_app.instance, nullptr);
        SetChildFont(g_app.wordWrap, font);
        Button_SetCheck(g_app.wordWrap, BST_CHECKED);

        RecreateStatusControl(true);
        ResetCurrentEffectiveState();

        RECT client{};
        GetClientRect(window, &client);
        LayoutControls(client.right - client.left, client.bottom - client.top);
        UpdateAcquisitionControls();

        g_app.capturingStartupDiagnostics = true;
        AppendStatusLine(L"ReflexProbe bootstrap: Launch, Watch or Attach selected API probes.");
        AppendStatusLine(L"Reflex probing is ON by default; DLSS FG probing is OFF. FG SetOptions is observed when enabled; GetState interception is independently optional.");
        AppendStatusLine(L"Frame-limit override is OFF by default. When checked, the FPS value replaces frameLimitUs on intercepted Reflex settings calls.");
        AppendStatusLine(L"Force Boost when Reflex On is independent: plain On requests become On + Boost; Off and existing On + Boost requests are unchanged.");
        AppendStatusLine(L"Reflex Sleep call counting is OFF by default. When disabled, ReflexProbe does not wrap the Reflex Sleep call.");
        AppendStatusLine(L"Capture mode defaults to State changes. Raw debug retains at most 131072 calls in RAM. ReflexProbe never writes capture data to disk.");
        LogBinaryIdentity();
        g_app.capturingStartupDiagnostics = false;
        SetTimer(window, kPollTimer, kPollIntervalMs, nullptr);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = MinimumWindowTrackWidth(window);
        info->ptMinTrackSize.y = kMinimumWindowHeight;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BROWSE:
            BrowseForGame();
            return 0;
        case IDC_GAME_PATH:
            if (HIWORD(wParam) == EN_KILLFOCUS)
                UpdateAcquisitionControls();
            return 0;
        case IDC_REFLEX_PROBING:
        case IDC_FG_PROBING:
            if (HIWORD(wParam) == BN_CLICKED) {
                UpdateAcquisitionControls();
                if (!g_app.process && !g_app.watchArmed && !g_app.haveEffectiveState)
                    ResetCurrentEffectiveState();
            }
            return 0;
        case IDC_OVERRIDE_ENABLE:
            if (HIWORD(wParam) == BN_CLICKED) {
                UpdateAcquisitionControls();
                ApplyPolicyToShared(true, false);
            }
            return 0;
        case IDC_OVERRIDE_FPS:
            if (HIWORD(wParam) == EN_KILLFOCUS &&
                Button_GetCheck(g_app.overrideEnable) == BST_CHECKED) {
                ApplyPolicyToShared(true, false);
            }
            return 0;
        case IDC_FORCE_BOOST:
            if (HIWORD(wParam) == BN_CLICKED)
                ApplyPolicyToShared(false, true);
            return 0;
        case IDC_COUNT_REFLEX_SLEEP:
        case IDC_FG_WRAP_GET_STATE:
            return 0;
        case IDC_FG_FORCE_AUTO_ON:
        case IDC_FG_MENU_OVERRIDE:
        case IDC_FG_MENU_OFF:
        case IDC_FG_MENU_ON:
        case IDC_FG_RETENTION_OVERRIDE:
        case IDC_FG_RETENTION_OFF:
        case IDC_FG_RETENTION_ON:
            if (HIWORD(wParam) == BN_CLICKED) {
                UpdateAcquisitionControls();
                ApplyFgPolicyToShared();
            }
            return 0;
        case IDC_LAUNCH:
            LaunchAndInject();
            return 0;
        case IDC_WATCH:
            ToggleWatchAndInject();
            return 0;
        case IDC_ATTACH:
            AttachToRunningTarget();
            return 0;
        case IDC_CAPTURE_STATE:
            if (HIWORD(wParam) == BN_CLICKED && Button_GetCheck(g_app.captureState) == BST_CHECKED)
                SetCaptureMode(CaptureModeStateChanges);
            return 0;
        case IDC_CAPTURE_RAW:
            if (HIWORD(wParam) == BN_CLICKED && Button_GetCheck(g_app.captureRaw) == BST_CHECKED)
                SetCaptureMode(CaptureModeRawDebug);
            return 0;
        case IDC_CLEAR_HISTORY:
            if (HIWORD(wParam) == BN_CLICKED)
                ClearCaptureHistory();
            return 0;
        case IDC_WORD_WRAP:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool wordWrap = Button_GetCheck(g_app.wordWrap) == BST_CHECKED;
                RecreateStatusControl(wordWrap);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == kPollTimer) {
            PollSharedState();
            return 0;
        }
        if (wParam == kWatchTimer) {
            PollProcessWatch();
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, kPollTimer);
        KillTimer(window, kWatchTimer);
        if (g_app.gamePath && g_app.gamePathOriginalProc) {
            SetWindowLongPtrW(g_app.gamePath, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_app.gamePathOriginalProc));
            g_app.gamePathOriginalProc = nullptr;
        }
        CleanupTarget();
        FreeRawDebugBuffer();
        FreeTextBuffer(g_app.startupDiagnostics);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void CenterWindowOnChosenMonitor(HWND window)
{
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect))
        return;

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;

    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;

    int x = monitorInfo.rcWork.left + (workWidth - width) / 2;
    int y = monitorInfo.rcWork.top + (workHeight - height) / 2;
    if (x < monitorInfo.rcWork.left)
        x = monitorInfo.rcWork.left;
    if (y < monitorInfo.rcWork.top)
        y = monitorInfo.rcWork.top;

    SetWindowPos(window, nullptr, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace ReflexProbeController

using namespace ReflexProbeController;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_app.instance = instance;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    HWND window = CreateWindowExW(0, kWindowClass, L"ReflexProbe",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;

    CenterWindowOnChosenMonitor(window);
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
