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

    if (g_app.launch)
        EnableWindow(g_app.launch, !targetActive && !watching);
    if (g_app.attach)
        EnableWindow(g_app.attach, !targetActive && !watching);
    if (g_app.watch) {
        EnableWindow(g_app.watch, !targetActive);
        SetWindowTextW(g_app.watch, watching ? L"Cancel Watch" : L"Watch + Inject");
    }

    if (g_app.gamePath)
        EnableWindow(g_app.gamePath, !watching);
    if (g_app.browse)
        EnableWindow(g_app.browse, !watching);
    if (g_app.arguments)
        EnableWindow(g_app.arguments, !watching);

    if (g_app.overrideEnable)
        EnableWindow(g_app.overrideEnable, !watching);
    if (g_app.overrideFps) {
        const bool frameOverrideChecked =
            g_app.overrideEnable && Button_GetCheck(g_app.overrideEnable) == BST_CHECKED;
        EnableWindow(g_app.overrideFps, !watching && frameOverrideChecked);
    }
    if (g_app.forceBoost)
        EnableWindow(g_app.forceBoost, !watching);
    if (g_app.countReflexSleep)
        EnableWindow(g_app.countReflexSleep, !targetActive && !watching);
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
    wchar_t error[512]{};
    if (!ParseOverrideFromUi(overrideEnabled, overrideUs, error, _countof(error))) {
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
            error, _countof(error))) {
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
    g_app.lastEventSerial = 0;
    g_app.loggedInjectedBuild = false;
    g_app.rawUiBatchWarningShown = false;
    UpdateAcquisitionControls();

    wchar_t line[512]{};
    swprintf_s(line, L"Launched %s (PID %lu) and injected ReflexProbe64.dll.", PathFileName(gamePath), process.dwProcessId);
    AppendStatusLine(line);
    if (overrideEnabled && overrideUs) {
        swprintf_s(line, L"Initial override: %u us (%.3f FPS).", overrideUs,
            1000000.0 / static_cast<double>(overrideUs));
        AppendStatusLine(line);
    } else if (overrideEnabled) {
        AppendStatusLine(L"Initial override: 0 us (no explicit Reflex frame limit).");
    } else {
        AppendStatusLine(L"Initial frame-limit override: disabled.");
    }

    if (forceBoostWhenOn)
        AppendStatusLine(L"Initial Force Boost policy: enabled; plain Reflex On requests will be forwarded as On + Boost.");
    else
        AppendStatusLine(L"Initial Force Boost policy: disabled; Reflex mode requests pass through unchanged.");

    AppendStatusLine(countReflexSleepCalls
        ? L"Initial Reflex Sleep call counting: enabled; Reflex Sleep will be wrapped for this target."
        : L"Initial Reflex Sleep call counting: disabled; Reflex Sleep will remain untouched for this target.");

    if (g_app.captureMode == CaptureModeRawDebug)
        AppendStatusLine(L"Capture mode: Raw debug; bounded raw retention starts with this target.");
    else
        AppendStatusLine(L"Capture mode: State changes; identical Reflex calls are counted but not retained individually.");

    return true;
}

bool ReadTargetAndPolicyForExternalAcquisition(
    wchar_t* gamePath, size_t gamePathCount,
    bool& overrideEnabled, uint32_t& overrideUs, bool& forceBoostWhenOn,
    bool& countReflexSleepCalls, wchar_t* error, size_t errorCount)
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

    forceBoostWhenOn = Button_GetCheck(g_app.forceBoost) == BST_CHECKED;
    countReflexSleepCalls =
        Button_GetCheck(g_app.countReflexSleep) == BST_CHECKED;
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
    wchar_t error[512]{};

    if (!ReadTargetAndPolicyForExternalAcquisition(
            gamePath, _countof(gamePath), overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    if (!ArmProcessWatch(gamePath, overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, error, _countof(error))) {
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
    wchar_t error[512]{};

    if (!ReadTargetAndPolicyForExternalAcquisition(
            gamePath, _countof(gamePath), overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, error, _countof(error))) {
        MessageBoxW(g_app.window, error, L"ReflexProbe", MB_ICONWARNING);
        return false;
    }

    if (!AttachRunningProcess(gamePath, overrideEnabled, overrideUs, forceBoostWhenOn,
            countReflexSleepCalls, error, _countof(error))) {
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

    if (GetOpenFileNameW(&dialog))
        SetWindowTextW(g_app.gamePath, path);
}

void LayoutControls(int clientWidth, int clientHeight)
{
    if (clientWidth <= 0 || clientHeight <= 0)
        return;

    const int usableWidth = clientWidth - (kMargin * 2);
    const int browseWidth = 96;
    const int launchWidth = 108;
    const int watchWidth = 116;
    const int attachWidth = 92;
    const int acquisitionGap = 8;
    const int wrapWidth = 104;
    const int rawWidth = 88;
    const int stateWidth = 108;
    const int captureLabelWidth = 92;
    const int captureGap = 8;

    if (g_app.gameLabel)
        MoveWindow(g_app.gameLabel, kMargin, 12, 150, 20, TRUE);
    if (g_app.browse)
        MoveWindow(g_app.browse, clientWidth - kMargin - browseWidth, 33, browseWidth, 26, TRUE);
    if (g_app.gamePath) {
        const int gameWidth = usableWidth - browseWidth - 10;
        MoveWindow(g_app.gamePath, kMargin, 34, gameWidth > 50 ? gameWidth : 50, 24, TRUE);
    }

    if (g_app.argumentsLabel)
        MoveWindow(g_app.argumentsLabel, kMargin, 68, 150, 20, TRUE);
    if (g_app.arguments)
        MoveWindow(g_app.arguments, kMargin, 90, usableWidth > 50 ? usableWidth : 50, 24, TRUE);

    if (g_app.overrideEnable)
        MoveWindow(g_app.overrideEnable, kMargin, 128, 172, 22, TRUE);
    if (g_app.overrideFps)
        MoveWindow(g_app.overrideFps, 184, 126, 70, 24, TRUE);
    if (g_app.fpsLabel)
        MoveWindow(g_app.fpsLabel, 262, 130, 34, 20, TRUE);
    if (g_app.forceBoost)
        MoveWindow(g_app.forceBoost, 306, 128, 205, 22, TRUE);
    if (g_app.countReflexSleep)
        MoveWindow(g_app.countReflexSleep, 515, 128, 190, 22, TRUE);
    const int attachX = clientWidth - kMargin - attachWidth;
    const int watchX = attachX - acquisitionGap - watchWidth;
    const int launchX = watchX - acquisitionGap - launchWidth;
    if (g_app.launch)
        MoveWindow(g_app.launch, launchX, 124, launchWidth, 30, TRUE);
    if (g_app.watch)
        MoveWindow(g_app.watch, watchX, 124, watchWidth, 30, TRUE);
    if (g_app.attach)
        MoveWindow(g_app.attach, attachX, 124, attachWidth, 30, TRUE);

    if (g_app.currentState)
        MoveWindow(g_app.currentState, kMargin, 164, usableWidth > 50 ? usableWidth : 50, 20, TRUE);

    if (g_app.statusLabel)
        MoveWindow(g_app.statusLabel, kMargin, 190, 180, 20, TRUE);

    const int wrapX = clientWidth - kMargin - wrapWidth;
    const int rawX = wrapX - captureGap - rawWidth;
    const int stateX = rawX - captureGap - stateWidth;
    const int captureLabelX = stateX - captureGap - captureLabelWidth;
    if (g_app.captureModeLabel)
        MoveWindow(g_app.captureModeLabel, captureLabelX, 190, captureLabelWidth, 20, TRUE);
    if (g_app.captureState)
        MoveWindow(g_app.captureState, stateX, 188, stateWidth, 22, TRUE);
    if (g_app.captureRaw)
        MoveWindow(g_app.captureRaw, rawX, 188, rawWidth, 22, TRUE);
    if (g_app.wordWrap)
        MoveWindow(g_app.wordWrap, wrapX, 188, wrapWidth, 22, TRUE);

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
            L"Current effective state: Reflex Unknown | Reflex FPS limit: Unknown",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window, nullptr, g_app.instance, nullptr);
        SetChildFont(g_app.currentState, font);

        g_app.statusLabel = CreateWindowExW(0, L"STATIC", L"Status / Reflex requests",
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

        AppendStatusLine(L"ReflexProbe bootstrap: Launch, Watch or Attach Reflex observer/override.");
        AppendStatusLine(L"Frame-limit override is OFF by default. When checked, the FPS value replaces frameLimitUs on intercepted Reflex settings calls.");
        AppendStatusLine(L"Force Boost when Reflex On is independent: plain On requests become On + Boost; Off and existing On + Boost requests are unchanged.");
        AppendStatusLine(L"Reflex Sleep call counting is OFF by default. When disabled, ReflexProbe does not wrap the Reflex Sleep call.");
        AppendStatusLine(L"Capture mode defaults to State changes. Raw debug retains at most 131072 calls in RAM. ReflexProbe never writes capture data to disk.");
        LogBinaryIdentity();
        SetTimer(window, kPollTimer, kPollIntervalMs, nullptr);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = kMinimumWindowWidth;
        info->ptMinTrackSize.y = kMinimumWindowHeight;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BROWSE:
            BrowseForGame();
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
        CleanupTarget();
        FreeRawDebugBuffer();
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
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 650,
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
