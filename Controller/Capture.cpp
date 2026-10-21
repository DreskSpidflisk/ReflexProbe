#include "ControllerInternal.h"

#include <windowsx.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>

namespace ReflexProbeController {

void ResetLiveStateTracking()
{
    g_app.haveStateEvent = false;
    g_app.stateEvent = {};
    g_app.stateRepeatCount = 0;
    g_app.stateSleepCount = 0;
    for (size_t i = 0; i < kFgTrackedStreams; ++i)
        g_app.fgRuns[i] = {};
    g_app.fgTrackingOverflowWarned = false;
    for (size_t i = 0; i < kPresentationTrackedStreams; ++i)
        g_app.presentationRuns[i] = {};
    g_app.presentationTrackingOverflowWarned = false;
}

bool EnsureRawDebugBuffer()
{
    if (g_app.rawDebugEvents)
        return true;

    const SIZE_T bytes = static_cast<SIZE_T>(kRawDebugCapacity * sizeof(CapturedReflexEvent));
    g_app.rawDebugEvents = static_cast<CapturedReflexEvent*>(
        HeapAlloc(GetProcessHeap(), 0, bytes));
    return g_app.rawDebugEvents != nullptr;
}

void ResetRawDebugCapture()
{
    g_app.rawDebugCount = 0;
    g_app.rawDebugWriteIndex = 0;
    g_app.rawDebugTotalCaptured = 0;
    g_app.rawDebugWrapped = false;
    g_app.rawRingWrapNoticePending = false;
    g_app.rawUiBatchWarningShown = false;
}

void FreeRawDebugBuffer()
{
    if (g_app.rawDebugEvents)
        HeapFree(GetProcessHeap(), 0, g_app.rawDebugEvents);
    g_app.rawDebugEvents = nullptr;
    ResetRawDebugCapture();
}

void ClearCaptureHistory()
{
    // Clear is deliberately idle-only. Never reset controller bookkeeping while
    // an injected target can still be publishing into the shared transport ring.
    if (g_app.process || g_app.watchArmed)
        return;

    FreeRawDebugBuffer();
    ResetLiveStateTracking();
    ResetCurrentEffectiveState();

    if (g_app.status) {
        SetWindowTextW(g_app.status, L"");
        // Restore only the startup banner, applying fresh timestamps to each line.
        // No captured events, session history or Clear marker are replayed.
        if (g_app.startupDiagnostics.data)
            AppendDisplayLineAtQpc(g_app.startupDiagnostics.data, 0);
    }
}

bool CaptureRawDebugEvent(const CapturedReflexEvent& event)
{
    if (!EnsureRawDebugBuffer())
        return false;

    g_app.rawDebugEvents[g_app.rawDebugWriteIndex] = event;
    g_app.rawDebugWriteIndex = (g_app.rawDebugWriteIndex + 1) % kRawDebugCapacity;
    ++g_app.rawDebugTotalCaptured;

    if (g_app.rawDebugCount < kRawDebugCapacity) {
        ++g_app.rawDebugCount;
    } else if (!g_app.rawDebugWrapped) {
        g_app.rawDebugWrapped = true;
        g_app.rawRingWrapNoticePending = true;
    }

    return true;
}

const wchar_t* BackendName(LONG backend)
{
    switch (backend) {
    case ReflexProbeProtocol::ReflexBackendModernSetOptions:
        return L"Streamline 2.x+ slReflexSetOptions";
    case ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants:
        return L"Streamline 1.x slSetFeatureConstants";
    case ReflexProbeProtocol::ReflexBackendLegacyPluginConstants:
        return L"Streamline 1.x sl.reflex plugin gateway";
    case ReflexProbeProtocol::ReflexBackendNativeNvapiD3D:
        return L"Native NVAPI D3D Reflex";
    default:
        return L"unknown";
    }
}

const wchar_t* BackendCallName(LONG backend)
{
    switch (backend) {
    case ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants:
        return L"slSetFeatureConstants";
    case ReflexProbeProtocol::ReflexBackendLegacyPluginConstants:
        return L"sl.reflex!slSetConstants";
    case ReflexProbeProtocol::ReflexBackendNativeNvapiD3D:
        return L"NvAPI_D3D_SetSleepMode";
    default:
        return L"slReflexSetOptions";
    }
}

const wchar_t* BackendSleepCallName(LONG backend)
{
    return backend == ReflexProbeProtocol::ReflexBackendNativeNvapiD3D
        ? L"NvAPI_D3D_Sleep"
        : L"slReflexSleep";
}

CaptureMode CurrentCaptureMode()
{
    return g_app.captureMode;
}

const wchar_t* ReflexModeName(LONG mode)
{
    switch (mode) {
    case 0:
        return L"Off";
    case 1:
        return L"On";
    case 2:
        return L"On + Boost";
    default:
        return L"Unknown";
    }
}

// Avoid needless Win32 STATIC/title changes even when different internal
// values round to identical display text. Hot-path Reflex events are already
// filtered by the effective mode/interval cache before reaching this helper.
void SetWindowTextIfChanged(HWND window, const wchar_t* desired)
{
    if (!window || !desired)
        return;

    const size_t desiredLength = wcslen(desired);
    if (desiredLength < 384 &&
        GetWindowTextLengthW(window) == static_cast<int>(desiredLength)) {
        wchar_t current[384]{};
        GetWindowTextW(window, current, static_cast<int>(_countof(current)));
        if (wcscmp(current, desired) == 0)
            return;
    }
    SetWindowTextW(window, desired);
}

void ResetCurrentEffectiveState()
{
    g_app.haveEffectiveState = false;
    g_app.currentEffectiveMode = 0;
    g_app.currentEffectiveUs = 0;
    g_app.haveFgLabelState = false;
    g_app.fgLabelMode = 0;
    g_app.fgLabelGeneratedFrames = 0;
    g_app.fgLabelMenuDetection = false;

    const bool reflexEnabled = g_app.reflexProbing &&
        Button_GetCheck(g_app.reflexProbing) == BST_CHECKED;
    const bool fgEnabled = g_app.fgProbing &&
        Button_GetCheck(g_app.fgProbing) == BST_CHECKED;

    SetWindowTextIfChanged(g_app.currentState,
        reflexEnabled ? L"Reflex: Unknown | Limit: Unknown"
            : L"Reflex: Probing disabled");
    SetWindowTextIfChanged(g_app.fgState,
        fgEnabled ? L"DLSS FG: Waiting for SetOptions"
            : L"DLSS FG: Probing disabled");
    SetWindowTextIfChanged(g_app.window,
        reflexEnabled ? L"ReflexProbe - Reflex Unknown | FPS Limit Unknown"
            : L"ReflexProbe - No Reflex probing");
}

void UpdateCurrentEffectiveState(const CapturedReflexEvent& event)
{
    if (event.result != 0)
        return;

    if (g_app.haveEffectiveState &&
        g_app.currentEffectiveMode == event.effectiveMode &&
        g_app.currentEffectiveUs == event.effectiveUs) {
        return;
    }

    g_app.haveEffectiveState = true;
    g_app.currentEffectiveMode = event.effectiveMode;
    g_app.currentEffectiveUs = event.effectiveUs;

    const wchar_t* mode = ReflexModeName(event.effectiveMode);
    wchar_t label[320]{};
    wchar_t title[224]{};

    if (event.effectiveUs) {
        const double fps = 1000000.0 / static_cast<double>(event.effectiveUs);
        swprintf_s(label,
            L"Reflex: %s | Limit: %.3f FPS",
            mode, fps);
        swprintf_s(title,
            L"ReflexProbe - Reflex %s | FPS Limit %.3f",
            mode, fps);
    } else {
        swprintf_s(label,
            L"Reflex: %s | Limit: None (0 us)",
            mode);
        swprintf_s(title,
            L"ReflexProbe - Reflex %s | FPS Limit None (0 us)",
            mode);
    }

    SetWindowTextIfChanged(g_app.currentState, label);
    SetWindowTextIfChanged(g_app.window, title);
}

void FormatValuePair(const wchar_t* field, const wchar_t* requested,
                     const wchar_t* forwarded, wchar_t* out, size_t outCount)
{
    if (wcscmp(requested, forwarded) == 0)
        swprintf_s(out, outCount, L"%s=%s", field, requested);
    else
        swprintf_s(out, outCount, L"%s=%s -> %s", field, requested, forwarded);
}

void FormatFrameLimitValue(uint32_t microseconds, wchar_t* out, size_t outCount)
{
    if (microseconds) {
        swprintf_s(out, outCount, L"%u us (%.3f FPS)", microseconds,
            1000000.0 / static_cast<double>(microseconds));
    } else {
        swprintf_s(out, outCount, L"0 us (no explicit frame limit)");
    }
}

void FormatReflexEvent(const CapturedReflexEvent& event, wchar_t* line, size_t lineCount)
{
    wchar_t mode[128]{};
    wchar_t requestedLimit[96]{};
    wchar_t forwardedLimit[96]{};
    wchar_t frameLimit[240]{};

    FormatValuePair(L"mode", ReflexModeName(event.requestedMode),
        ReflexModeName(event.effectiveMode), mode, _countof(mode));
    FormatFrameLimitValue(event.requestedUs, requestedLimit, _countof(requestedLimit));
    FormatFrameLimitValue(event.effectiveUs, forwardedLimit, _countof(forwardedLimit));
    FormatValuePair(L"frameLimit", requestedLimit, forwardedLimit,
        frameLimit, _countof(frameLimit));

    swprintf_s(line, lineCount, L"%s #%ld: %s, %s, result=%ld",
        BackendCallName(event.backend), event.sequence, mode, frameLimit, event.result);
}

void FormatReflexDisplayLine(const CapturedReflexEvent& event, bool newState,
                            wchar_t* line, size_t lineCount)
{
    wchar_t raw[384]{};
    FormatReflexEvent(event, raw, _countof(raw));
    if (newState)
        swprintf_s(line, lineCount, L"NEW Reflex State: %s", raw);
    else
        wcscpy_s(line, lineCount, raw);
}

void FormatSleepDisplayLine(const CapturedReflexEvent& event,
                            wchar_t* line, size_t lineCount)
{
    swprintf_s(line, lineCount, L"%s #%ld: result=%ld",
        BackendSleepCallName(event.backend), event.sequence, event.result);
}

void AppendReflexEvent(const CapturedReflexEvent& event, bool newState)
{
    wchar_t line[512]{};
    FormatReflexDisplayLine(event, newState, line, _countof(line));
    AppendDisplayLineAtQpc(line, event.qpc);
}


const wchar_t* FgModeName(uint32_t mode)
{
    switch (mode) {
    case 0: return L"Off";
    case 1: return L"On";
    case 2: return L"Auto";
    case 3: return L"Dynamic";
    default: return L"Unknown";
    }
}

const wchar_t* FgBooleanName(uint32_t value)
{
    if (value == 0)
        return L"No";
    if (value == 1)
        return L"Yes";
    return L"Unknown";
}

void FormatFgDisplayLine(const CapturedReflexEvent& event, bool newState,
                         wchar_t* line, size_t lineCount, bool detailed = true)
{
    const ReflexProbeProtocol::FgEventData& fg = event.fg;
    const wchar_t* prefix = !newState ? L"" :
        (event.kind == ReflexProbeProtocol::FgEventSetOptions
            ? L"NEW FG SetOptions: " : L"NEW FG GetState: ");
    if (event.kind == ReflexProbeProtocol::FgEventSetOptions) {
        wchar_t requestedMode[64]{};
        wchar_t forwardedMode[64]{};
        wchar_t mode[160]{};
        swprintf_s(requestedMode, L"%s (%u)", FgModeName(fg.mode), fg.mode);
        swprintf_s(forwardedMode, L"%s (%u)", FgModeName(fg.forwardedMode), fg.forwardedMode);
        FormatValuePair(L"mode", requestedMode, forwardedMode, mode, _countof(mode));

        wchar_t requestedFrames[48]{};
        wchar_t forwardedFrames[48]{};
        wchar_t frames[128]{};
        swprintf_s(requestedFrames, L"%u (%uX)", fg.generatedFrames, fg.generatedFrames + 1);
        swprintf_s(forwardedFrames, L"%u (%uX)",
            fg.forwardedGeneratedFrames, fg.forwardedGeneratedFrames + 1);
        FormatValuePair(L"framesToGenerate", requestedFrames, forwardedFrames,
            frames, _countof(frames));

        wchar_t menuDetection[96]{};
        FormatValuePair(L"fullscreenMenuDetection",
            (fg.flags & 0x10u) ? L"On" : L"Off",
            (fg.forwardedFlags & 0x10u) ? L"On" : L"Off",
            menuDetection, _countof(menuDetection));

        wchar_t retention[96]{};
        FormatValuePair(L"retainResourcesWhenOff",
            (fg.flags & 0x08u) ? L"On" : L"Off",
            (fg.forwardedFlags & 0x08u) ? L"On" : L"Off",
            retention, _countof(retention));

        if (!detailed) {
            swprintf_s(line, lineCount,
                L"%sslDLSSGSetOptions #%ld: %s, %s, %s, %s, result=%ld",
                prefix, event.sequence, mode, frames, menuDetection, retention, event.result);
            return;
        }

        wchar_t requestedFlags[24]{};
        wchar_t forwardedFlags[24]{};
        wchar_t flags[96]{};
        swprintf_s(requestedFlags, L"0x%08X", fg.flags);
        swprintf_s(forwardedFlags, L"0x%08X", fg.forwardedFlags);
        FormatValuePair(L"flags", requestedFlags, forwardedFlags, flags, _countof(flags));

        float targetFps = 0;
        memcpy(&targetFps, &fg.targetFrameRateBits, sizeof(targetFps));
        const wchar_t* overrideMarker =
            (fg.mode != fg.forwardedMode ||
             fg.generatedFrames != fg.forwardedGeneratedFrames ||
             fg.flags != fg.forwardedFlags)
                ? L" [OVERRIDE]" : L"";
        wchar_t extras[256]{};
        if (fg.optionsVersion >= 3)
            swprintf_s(extras, L", queueParallelism=%u", fg.parallelism);
        if (fg.optionsVersion >= 4) {
            wchar_t tail[100]{};
            swprintf_s(tail, L", UIRecomposition=%s", FgBooleanName(fg.uiRecomposition));
            wcscat_s(extras, tail);
        }
        if (fg.optionsVersion >= 5) {
            wchar_t tail[100]{};
            swprintf_s(tail, L", dynamicTargetFPS=%.3f", targetFps);
            wcscat_s(extras, tail);
        }
        swprintf_s(line, lineCount,
            L"%sslDLSSGSetOptions #%ld: viewport=%u, version=%u, "
            L"%s, %s, %s%s "
            L"[%s, %s, "
            L"dynamicResolution=%s, showOnlyInterpolated=%s, requestVRAM=%s], "
            L"dynamicRes=%ux%u, backBuffers=%u, inputSize=%ux%u, backbufferSize=%ux%u, "
            L"bufferFormats[color=%u mvec=%u depth=%u hudless=%u UI=%u], "
            L"errorCallback=%s%s, result=%ld",
            prefix, event.sequence, fg.viewport, fg.optionsVersion,
            mode, frames, flags, overrideMarker,
            menuDetection, retention,
            (fg.flags & 0x02u) ? L"On" : L"Off",
            (fg.flags & 0x01u) ? L"On" : L"Off",
            (fg.flags & 0x04u) ? L"On" : L"Off",
            fg.dynamicWidth, fg.dynamicHeight, fg.numBackBuffers,
            fg.motionDepthWidth, fg.motionDepthHeight, fg.colorWidth, fg.colorHeight,
            fg.colorBufferFormat, fg.motionBufferFormat, fg.depthBufferFormat,
            fg.hudlessBufferFormat, fg.uiBufferFormat,
            fg.errorCallbackPresent ? L"Present" : L"None",
            extras, event.result);
    } else {
        wchar_t options[240]{};
        // Query inputs belong in Raw debug; State changes describes returned
        // persistent state, never the arguments of the query that obtained it.
        if (fg.optionsPresent && !newState) {
            swprintf_s(options,
                L", inputOptions[v%u mode=%s (%u), framesToGenerate=%u, "
                L"flags=0x%08X, fullscreenMenuDetection=%s]",
                fg.optionsVersion, FgModeName(fg.mode), fg.mode,
                fg.generatedFrames, fg.flags,
                (fg.flags & 0x10u) ? L"On" : L"Off");
        }
        if (!fg.stateValid) {
            swprintf_s(line, lineCount,
                L"%sslDLSSGGetState #%ld: viewport=%u, stateVersion=%u%s, result=%ld "
                L"(state unavailable; output not interpreted)",
                prefix, event.sequence, fg.viewport, fg.stateVersion, options, event.result);
        } else {
            wchar_t stateExtras[240]{};
            if (fg.stateVersion >= 2)
                swprintf_s(stateExtras,
                    L", maxFramesToGenerate=%u (%uX max), VSyncAvailable=%s",
                    fg.maxGeneratedFrames, fg.maxGeneratedFrames + 1,
                    FgBooleanName(fg.vsyncAvailable));
            if (fg.stateVersion >= 4) {
                wchar_t tail[80]{};
                swprintf_s(tail, L", dynamicMFGSupported=%s",
                    FgBooleanName(fg.dynamicMfgAvailable));
                wcscat_s(stateExtras, tail);
            }
            swprintf_s(line, lineCount,
                L"%sslDLSSGGetState #%ld: viewport=%u, stateVersion=%u, "
                L"status=0x%08X (%s), minDimension=%u%s, "
                L"framesPresentedSincePreviousGetState=%u [transient]%s, result=%ld",
                prefix, event.sequence, fg.viewport, fg.stateVersion,
                fg.status, fg.status ? L"status flags set" : L"OK",
                fg.minDimension, stateExtras, fg.framesPresented, options, event.result);
        }
    }
}

void AppendFgEvent(const CapturedReflexEvent& event, bool newState, bool detailed = true)
{
    wchar_t line[1024]{};
    FormatFgDisplayLine(event, newState, line, _countof(line), detailed);
    AppendDisplayLineAtQpc(line, event.qpc);
}

void AppendFgRepeatCount(const FgStateRun& run, LONGLONG eventQpc)
{
    const bool normalSetOptions =
        run.kind == ReflexProbeProtocol::FgEventSetOptions &&
        !g_app.fgDetailedDiagnostics;
    const uint64_t count = normalSetOptions ? run.pendingDisplayRepeats : run.repeats;
    if (!count)
        return;

    wchar_t line[180]{};
    const bool isSetOptions = run.kind == ReflexProbeProtocol::FgEventSetOptions;
    const wchar_t* callName = isSetOptions ? L"slDLSSGSetOptions" : L"slDLSSGGetState";
    if (normalSetOptions) {
        // Exact complete-state repetitions, potentially across multiple
        // hidden state transitions; do not claim a single state persisted.
        swprintf_s(line,
            L"PREVIOUS FG SetOptions: %llu repeated calls since %s #%ld (viewport=%u).",
            static_cast<unsigned long long>(count),
            callName, run.displayed.sequence, run.viewport);
    } else {
        swprintf_s(line,
            L"PREVIOUS FG %s: %llu repeated calls since %s #%ld (viewport=%u).",
            isSetOptions ? L"SetOptions" : L"GetState",
            static_cast<unsigned long long>(count),
            callName, run.displayed.sequence, run.viewport);
    }
    AppendDisplayLineAtQpc(line, eventQpc);
}

void AppendFgSnapshot(const FgStateRun& run, LONGLONG eventQpc)
{
    if (run.kind != ReflexProbeProtocol::FgEventSetOptions)
        return;
    wchar_t state[1024]{};
    wchar_t line[1152]{};
    FormatFgDisplayLine(run.previous, false, state, _countof(state),
        g_app.fgDetailedDiagnostics);
    swprintf_s(line, L"FG State snapshot: %s", state);
    AppendDisplayLineAtQpc(line, eventQpc);
}

void FinishFgDisplayIntervals(LONGLONG eventQpc, bool includeGetState)
{
    for (size_t i = 0; i < kFgTrackedStreams; ++i) {
        FgStateRun& run = g_app.fgRuns[i];
        if (!run.used || (!includeGetState &&
            run.kind != ReflexProbeProtocol::FgEventSetOptions))
            continue;
        AppendFgRepeatCount(run, eventQpc);
        run.repeats = 0;
        run.pendingDisplayRepeats = 0;
        run.displayed = run.previous;
    }
}

void SetFgDetailedDiagnostics(bool detailed)
{
    if (g_app.fgDetailedDiagnostics == detailed)
        return;
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    if (g_app.captureMode == CaptureModeStateChanges)
        FinishFgDisplayIntervals(qpc.QuadPart, false);
    g_app.fgDetailedDiagnostics = detailed;
    AppendStatusLineAtQpc(detailed
        ? L"Detailed FG diagnostics enabled."
        : L"Detailed FG diagnostics disabled; normal FG SetOptions view active.",
        qpc.QuadPart);
    if (g_app.captureMode == CaptureModeStateChanges) {
        for (size_t i = 0; i < kFgTrackedStreams; ++i) {
            if (g_app.fgRuns[i].used)
                AppendFgSnapshot(g_app.fgRuns[i], qpc.QuadPart);
        }
    }
}

bool SameVisibleFgSetOptions(const CapturedReflexEvent& a, const CapturedReflexEvent& b)
{
    constexpr uint32_t kVisibleFlags = 0x10u | 0x08u;
    return a.result == b.result &&
           a.fg.mode == b.fg.mode &&
           a.fg.forwardedMode == b.fg.forwardedMode &&
           a.fg.generatedFrames == b.fg.generatedFrames &&
           a.fg.forwardedGeneratedFrames == b.fg.forwardedGeneratedFrames &&
           (a.fg.flags & kVisibleFlags) == (b.fg.flags & kVisibleFlags) &&
           (a.fg.forwardedFlags & kVisibleFlags) ==
               (b.fg.forwardedFlags & kVisibleFlags);
}

bool SameFgState(const CapturedReflexEvent& a, const CapturedReflexEvent& b)
{
    if (a.kind != b.kind || a.result != b.result)
        return false;

    if (a.kind == ReflexProbeProtocol::FgEventGetState) {
        const ReflexProbeProtocol::FgEventData& left = a.fg;
        const ReflexProbeProtocol::FgEventData& right = b.fg;
        // GetState's optional DLSSGOptions are *query inputs*. Alternating
        // calls with options and nullptr (as DOOM: The Dark Ages does) must
        // not create a new runtime state. Compare only captured output fields.
        // framesPresented is transient since the preceding GetState call.
        if (left.viewport != right.viewport ||
            left.stateVersion != right.stateVersion ||
            left.stateValid != right.stateValid)
            return false;

        // An unsuccessful query has no valid output. The result code above
        // still distinguishes errors; caller inputs never do.
        if (!left.stateValid)
            return true;

        if (left.status != right.status ||
            left.minDimension != right.minDimension)
            return false;

        if (left.stateVersion >= 2 &&
            (left.maxGeneratedFrames != right.maxGeneratedFrames ||
             left.vsyncAvailable != right.vsyncAvailable))
            return false;

        return left.stateVersion < 4 ||
            left.dynamicMfgAvailable == right.dynamicMfgAvailable;
    }

    // SetOptions is a requested configuration, so all its input fields are
    // meaningful for change detection. Keep that existing behavior.
    return memcmp(&a.fg, &b.fg, sizeof(a.fg)) == 0;
}

void HandleCapturedFgEvent(const CapturedReflexEvent& event)
{
    const bool raw = CurrentCaptureMode() == CaptureModeRawDebug;
    if (raw)
        CaptureRawDebugEvent(event);

    FgStateRun* available = nullptr;
    for (size_t i = 0; i < kFgTrackedStreams; ++i) {
        FgStateRun& run = g_app.fgRuns[i];
        if (run.used && run.kind == event.kind &&
            run.viewport == event.fg.viewport) {
            if (raw) {
                // Preserve latest complete state through Raw without
                // incurring any formatting or state-change reporting cost.
                run.previous = event;
                run.displayed = event;
                return;
            }

            if (SameFgState(run.previous, event)) {
                ++run.repeats;
                // Keep snapshots' call ordinal current without copying the
                // entire event or changing the full-state comparison.
                run.previous.sequence = event.sequence;
                if (run.kind == ReflexProbeProtocol::FgEventSetOptions)
                    ++run.pendingDisplayRepeats;
            } else {
                const bool showState =
                    run.kind != ReflexProbeProtocol::FgEventSetOptions ||
                    g_app.fgDetailedDiagnostics ||
                    !SameVisibleFgSetOptions(run.displayed, event);
                if (showState) {
                    AppendFgRepeatCount(run, event.qpc);
                    AppendFgEvent(event, true, g_app.fgDetailedDiagnostics);
                    run.displayed = event;
                    run.pendingDisplayRepeats = 0;
                }
                // Hidden changes still update the true full-state baseline.
                run.previous = event;
                run.repeats = 0;
            }
            return;
        }
        if (!run.used && !available)
            available = &run;
    }

    if (!available) {
        if (!raw && !g_app.fgTrackingOverflowWarned) {
            g_app.fgTrackingOverflowWarned = true;
            AppendStatusLine(L"WARNING: FG tracker exceeded 32 viewport/function streams; "
                L"excess streams are logged uncompressed.");
        }
        if (!raw)
            AppendFgEvent(event, true, g_app.fgDetailedDiagnostics);
        return;
    }

    available->used = true;
    available->kind = event.kind;
    available->viewport = event.fg.viewport;
    available->previous = event;
    available->displayed = event;
    available->repeats = 0;
    available->pendingDisplayRepeats = 0;
    if (!raw)
        AppendFgEvent(event, true, g_app.fgDetailedDiagnostics);
}

void UpdateCurrentFgState(const CapturedReflexEvent& event)
{
    // GetState reports query results, not the game's requested FG engagement.
    // Keep the status label exclusively tied to SetOptions.
    if (event.kind != ReflexProbeProtocol::FgEventSetOptions || !g_app.fgState)
        return;

    const ReflexProbeProtocol::FgEventData& fg = event.fg;
    const bool menuDetection = (fg.forwardedFlags & 0x10u) != 0;
    if (g_app.haveFgLabelState &&
        g_app.fgLabelMode == fg.forwardedMode &&
        g_app.fgLabelGeneratedFrames == fg.forwardedGeneratedFrames &&
        g_app.fgLabelMenuDetection == menuDetection)
        return;

    wchar_t label[320]{};
    swprintf_s(label, L"DLSS FG: %s | %uX | Menu detection: %s",
        FgModeName(fg.forwardedMode), fg.forwardedGeneratedFrames + 1,
        menuDetection ? L"On" : L"Off");
    if (SetWindowTextW(g_app.fgState, label)) {
        g_app.haveFgLabelState = true;
        g_app.fgLabelMode = fg.forwardedMode;
        g_app.fgLabelGeneratedFrames = fg.forwardedGeneratedFrames;
        g_app.fgLabelMenuDetection = menuDetection;
    }
}

bool AppendCapturedEventToBuffer(TextBuffer& buffer, const CapturedReflexEvent& event, bool newState)
{
    wchar_t line[512]{};
    if (event.kind == ReflexProbeProtocol::PresentationEventPresent ||
        event.kind == ReflexProbeProtocol::PresentationEventSwapChain) {
        FormatPresentationEvent(event, line, _countof(line));
        return AppendTextBufferLineAtQpc(buffer, line, event.qpc);
    }
    if (event.kind == ReflexProbeProtocol::FgEventSetOptions ||
        event.kind == ReflexProbeProtocol::FgEventGetState) {
        wchar_t fgLine[1024]{};
        FormatFgDisplayLine(event, newState, fgLine, _countof(fgLine));
        return AppendTextBufferLineAtQpc(buffer, fgLine, event.qpc);
    }
    if (event.kind == ReflexProbeProtocol::ReflexEventSleep)
        FormatSleepDisplayLine(event, line, _countof(line));
    else
        FormatReflexDisplayLine(event, newState, line, _countof(line));
    return AppendTextBufferLineAtQpc(buffer, line, event.qpc);
}

bool SameReflexState(const CapturedReflexEvent& a, const CapturedReflexEvent& b)
{
    return a.backend == b.backend &&
           a.requestedMode == b.requestedMode &&
           a.effectiveMode == b.effectiveMode &&
           a.requestedUs == b.requestedUs &&
           a.effectiveUs == b.effectiveUs &&
           a.result == b.result;
}

void FormatRepeatCount(uint64_t repeatCount, const CapturedReflexEvent& state,
                       wchar_t* line, size_t lineCount)
{
    swprintf_s(line, lineCount, L"PREVIOUS Reflex State: %llu repeated calls since %s #%ld.",
        static_cast<unsigned long long>(repeatCount),
        BackendCallName(state.backend), state.sequence);
}

void AppendRepeatCount(uint64_t repeatCount, const CapturedReflexEvent& state,
                       LONGLONG eventQpc)
{
    if (!repeatCount)
        return;

    wchar_t line[180]{};
    FormatRepeatCount(repeatCount, state, line, _countof(line));
    AppendDisplayLineAtQpc(line, eventQpc);
}

bool SleepCountingCurrentlyEnabled()
{
    return g_app.countReflexSleep &&
           Button_GetCheck(g_app.countReflexSleep) == BST_CHECKED;
}

void AppendSleepCount(uint64_t sleepCount, const CapturedReflexEvent& state,
                      LONGLONG eventQpc)
{
    // SL1 has no sleep wrapper in ReflexProbe. Never present unavailable
    // measurement as a zero; zero only becomes meaningful after a sleep
    // event has actually been intercepted for this target.
    if (!g_app.shared)
        return;

    const LONG backend = InterlockedCompareExchange(&g_app.shared->backend, 0, 0);
    if (backend == ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants ||
        backend == ReflexProbeProtocol::ReflexBackendLegacyPluginConstants)
        return;

    if (!sleepCount &&
        (!SleepCountingCurrentlyEnabled() ||
         InterlockedCompareExchange(&g_app.shared->sleepCallSerial, 0, 0) == 0))
        return;

    wchar_t line[180]{};
    swprintf_s(line, L"Reflex Sleep: %llu calls since %s #%ld.",
        static_cast<unsigned long long>(sleepCount),
        BackendCallName(state.backend), state.sequence);
    AppendDisplayLineAtQpc(line, eventQpc);
}

void FinalizeStateRun(LONGLONG eventQpc)
{
    if (g_app.haveStateEvent) {
        AppendRepeatCount(g_app.stateRepeatCount, g_app.stateEvent, eventQpc);
        AppendSleepCount(g_app.stateSleepCount, g_app.stateEvent, eventQpc);
    }
    for (size_t i = 0; i < kFgTrackedStreams; ++i) {
        if (g_app.fgRuns[i].used)
            AppendFgRepeatCount(g_app.fgRuns[i], eventQpc);
    }
    for (const auto& run : g_app.presentationRuns)
        if (run.used)
            AppendPresentationRepeat(run, eventQpc);
    ResetLiveStateTracking();
}

void SetCaptureButtons(CaptureMode mode)
{
    if (g_app.captureState)
        Button_SetCheck(g_app.captureState, mode == CaptureModeStateChanges ? BST_CHECKED : BST_UNCHECKED);
    if (g_app.captureRaw)
        Button_SetCheck(g_app.captureRaw, mode == CaptureModeRawDebug ? BST_CHECKED : BST_UNCHECKED);
}

bool SetCaptureMode(CaptureMode mode)
{
    if (mode == g_app.captureMode) {
        SetCaptureButtons(mode);
        return true;
    }

    // Consume already-published events under the old capture policy.
    PollSharedState();
    if (mode == CaptureModeRawDebug && !EnsureRawDebugBuffer()) {
        MessageBoxW(g_app.window,
            L"Could not allocate the bounded Raw debug capture buffer.",
            L"ReflexProbe", MB_ICONERROR);
        SetCaptureButtons(g_app.captureMode);
        return false;
    }

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const CaptureMode previous = g_app.captureMode;
    if (previous == CaptureModeStateChanges) {
        if (g_app.haveStateEvent) {
            AppendRepeatCount(g_app.stateRepeatCount, g_app.stateEvent, qpc.QuadPart);
            AppendSleepCount(g_app.stateSleepCount, g_app.stateEvent, qpc.QuadPart);
        }
        FinishFgDisplayIntervals(qpc.QuadPart, true);
        for (const auto& run : g_app.presentationRuns)
            if (run.used)
                AppendPresentationRepeat(run, qpc.QuadPart);
    }

    // Reset Reflex presentation, not the last observed complete FG state.
    // Raw mode continues updating that FG state as calls arrive.
    g_app.haveStateEvent = false;
    g_app.stateRepeatCount = 0;
    g_app.stateSleepCount = 0;
    for (auto& run : g_app.presentationRuns)
        run = {};
    g_app.presentationTrackingOverflowWarned = false;
    if (mode == CaptureModeRawDebug)
        ResetRawDebugCapture();

    g_app.captureMode = mode;
    SetCaptureButtons(mode);
    UpdateAcquisitionControls();

    if (mode == CaptureModeRawDebug) {
        AppendStatusLineAtQpc(
            L"Capture mode changed to Raw debug. Raw retention starts now; earlier calls are not reconstructed.",
            qpc.QuadPart);
    } else {
        wchar_t line[224]{};
        swprintf_s(line,
            L"Capture mode changed to State changes. Raw debug stopped after %llu captured call(s); current FG states shown as snapshots.",
            static_cast<unsigned long long>(g_app.rawDebugTotalCaptured));
        AppendStatusLineAtQpc(line, qpc.QuadPart);
        for (size_t i = 0; i < kFgTrackedStreams; ++i) {
            FgStateRun& run = g_app.fgRuns[i];
            if (!run.used)
                continue;
            run.repeats = 0;
            run.pendingDisplayRepeats = 0;
            run.displayed = run.previous;
            AppendFgSnapshot(run, qpc.QuadPart);
        }
    }

    return true;
}
void HandleCapturedReflexEvent(const CapturedReflexEvent& event)
{
    if (event.kind == ReflexProbeProtocol::PresentationEventPresent ||
        event.kind == ReflexProbeProtocol::PresentationEventSwapChain) {
        if (CurrentCaptureMode() == CaptureModeRawDebug)
            CaptureRawDebugEvent(event);
        else
            HandlePresentationEvent(event);
        return;
    }
    if (event.kind == ReflexProbeProtocol::FgEventSetOptions ||
        event.kind == ReflexProbeProtocol::FgEventGetState) {
        HandleCapturedFgEvent(event);
        return;
    }

    if (CurrentCaptureMode() == CaptureModeRawDebug) {
        CaptureRawDebugEvent(event);
        return;
    }

    if (event.kind == ReflexProbeProtocol::ReflexEventSleep) {
        if (g_app.haveStateEvent)
            ++g_app.stateSleepCount;
        return;
    }

    if (!g_app.haveStateEvent) {
        g_app.haveStateEvent = true;
        g_app.stateEvent = event;
        g_app.stateRepeatCount = 0;
        g_app.stateSleepCount = 0;
        AppendReflexEvent(event, true);
    } else if (SameReflexState(g_app.stateEvent, event)) {
        ++g_app.stateRepeatCount;
    } else {
        AppendRepeatCount(g_app.stateRepeatCount, g_app.stateEvent, event.qpc);
        AppendSleepCount(g_app.stateSleepCount, g_app.stateEvent, event.qpc);
        AppendReflexEvent(event, true);
        g_app.stateEvent = event;
        g_app.stateRepeatCount = 0;
        g_app.stateSleepCount = 0;
    }
}
void RecordRunBoundary(LONGLONG eventQpc)
{
    if (CurrentCaptureMode() == CaptureModeStateChanges)
        FinalizeStateRun(eventQpc);
    else
        ResetLiveStateTracking();
}

void PollSharedState()
{
    if (!g_app.process || !g_app.shared)
        return;

    const bool targetExited = WaitForSingleObject(g_app.process, 0) == WAIT_OBJECT_0;
    const bool rawDebug = CurrentCaptureMode() == CaptureModeRawDebug;
    TextBuffer rawBatch{};
    bool rawBatchSuccess = true;

    if (!g_app.loggedInjectedBuild && g_app.shared->injectedBuild[0]) {
        wchar_t line[256]{};
        swprintf_s(line, L"Injected DLL reports build: %s", g_app.shared->injectedBuild);
        AppendStatusLine(line);
        g_app.loggedInjectedBuild = true;
    }

    const LONG backend = InterlockedCompareExchange(&g_app.shared->backend, 0, 0);
    const LONG hookState = InterlockedCompareExchange(&g_app.shared->hookState, 0, 0);
    if (hookState != g_app.lastHookState) {
        const LONG previousHookState = g_app.lastHookState;
        g_app.lastHookState = hookState;
        const bool warnLegacySleep =
            g_app.shared->countReflexSleepCalls != 0 &&
            (backend == ReflexProbeProtocol::ReflexBackendLegacyFeatureConstants ||
             backend == ReflexProbeProtocol::ReflexBackendLegacyPluginConstants);
        const wchar_t* legacySleepNote = warnLegacySleep
            ? L"\r\nNOTE: Reflex Sleep counting unavailable for Streamline 1.x (not intercepted by ReflexProbe)."
            : L"";
        wchar_t line[2300]{};
        switch (hookState) {
        case ReflexProbeProtocol::HookStateWaitingForDll:
            wcscpy_s(line, g_app.shared->probeReflexEnabled
                ? L"Waiting for a Reflex interception path..."
                : g_app.shared->probeDlssFgEnabled
                    ? L"Waiting for a Streamline 2.x DLSS FG interception path."
                    : L"Waiting for a DXGI swap-chain creation / Present path...");
            break;
        case ReflexProbeProtocol::HookStateInterceptArmed:
            swprintf_s(line, L"Interception armed: %s. Waiting for sl.reflex.dll to resolve its plugin gateway.",
                BackendName(backend));
            break;
        case ReflexProbeProtocol::HookStateReflexFound:
            swprintf_s(line, L"Found Reflex backend: %s%s\r\nModule: %s\r\nFile version: %s",
                BackendName(backend), legacySleepNote,
                g_app.shared->reflexPath[0] ? g_app.shared->reflexPath : L"(path unavailable)",
                g_app.shared->reflexVersion[0] ? g_app.shared->reflexVersion : L"unknown");
            break;
        case ReflexProbeProtocol::HookStateHooked:
            swprintf_s(line, L"Hook active: %s via %s%s\r\nModule: %s\r\nFile version: %s",
                BackendCallName(backend), BackendName(backend),
                previousHookState == ReflexProbeProtocol::HookStateReflexFound
                    ? L"" : legacySleepNote,
                g_app.shared->reflexPath[0] ? g_app.shared->reflexPath : L"sl.reflex.dll",
                g_app.shared->reflexVersion[0] ? g_app.shared->reflexVersion : L"unknown");
            break;
        case ReflexProbeProtocol::HookStateError:
            swprintf_s(line, L"Injected DLL error: %s",
                g_app.shared->lastError[0] ? g_app.shared->lastError : L"unknown error");
            break;
        default:
            swprintf_s(line, L"Unknown hook state: %ld", hookState);
            break;
        }
        AppendStatusLine(line);
    }

    const LONG fgHookBits = InterlockedCompareExchange(&g_app.shared->fgHookBits, 0, 0);
    const LONG newFgHooks = fgHookBits & ~g_app.lastFgHookBits;
    if (newFgHooks) {
        if (newFgHooks & 1) {
            wchar_t line[1500]{};
            swprintf_s(line, L"FG hook active: slDLSSGSetOptions\r\nModule: %s\r\nVersion: %s\r\nMethod: %s",
                g_app.shared->fgPath[0] ? g_app.shared->fgPath : L"(unknown)",
                g_app.shared->fgVersion[0] ? g_app.shared->fgVersion : L"(unknown)",
                g_app.shared->fgMethod[0] ? g_app.shared->fgMethod : L"(unknown)");
            AppendStatusLine(line);
        }
        if (newFgHooks & 2) {
            AppendStatusLine(L"FG hook active: slDLSSGGetState (runtime-reported state now observable).");
        }
        g_app.lastFgHookBits = fgHookBits;
    }

    const LONG presentationBits = InterlockedCompareExchange(
        &g_app.shared->presentationHookBits, 0, 0);
    const LONG newPresentationHooks = presentationBits & ~g_app.lastPresentationHookBits;
    if (newPresentationHooks) {
        if (newPresentationHooks & 1)
            AppendStatusLine(L"DXGI factory interception active; waiting for a swap chain.");
        if (newPresentationHooks & 2)
            AppendStatusLine(L"DXGI Present interception active (observation only).");
        if (newPresentationHooks & 4)
            AppendStatusLine(L"DXGI Present1 interception active (observation only).");
        if (newPresentationHooks & 8)
            AppendStatusLine(L"DXGI ResizeBuffers interception active (observation only).");
        if (newPresentationHooks & 16)
            AppendStatusLine(L"D3D11 direct device-and-swap-chain creation intercepted.");
        if (newPresentationHooks & 32)
            AppendStatusLine(L"WARNING: Recursive DXGI factory forwarding detected; aborted a nested call. Check Steam overlay chaining.");
        g_app.lastPresentationHookBits = presentationBits;
    }

    const LONG currentSerial = InterlockedCompareExchange(&g_app.shared->eventSerial, 0, 0);
    LONG first = g_app.lastEventSerial + 1;
    const LONG available = currentSerial - first + 1;
    if (available > static_cast<LONG>(ReflexProbeProtocol::kEventCapacity)) {
        const LONG lost = available - static_cast<LONG>(ReflexProbeProtocol::kEventCapacity);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        RecordRunBoundary(qpc.QuadPart);

        wchar_t line[256]{};
        swprintf_s(line,
            L"WARNING: Probe transport ring overrun; %ld call(s) were lost before the controller drained them. Capture continuity is broken at this point.",
            lost);
        AppendStatusLineAtQpc(line, qpc.QuadPart);

        first = currentSerial - static_cast<LONG>(ReflexProbeProtocol::kEventCapacity) + 1;
        g_app.lastEventSerial = first - 1;
    }

    LONG processedSerial = g_app.lastEventSerial;
    for (LONG serial = first; serial <= currentSerial; ++serial) {
        if (serial <= 0)
            continue;

        const uint32_t index = static_cast<uint32_t>(serial - 1) % ReflexProbeProtocol::kEventCapacity;
        const ReflexProbeProtocol::ReflexEvent& event = g_app.shared->events[index];
        const LONG storedSerial = InterlockedCompareExchange(
            const_cast<volatile LONG*>(&event.sequence), 0, 0);
        if (storedSerial != serial)
            break;

        CapturedReflexEvent captured{};
        captured.qpc = event.qpc;
        captured.sequence = event.callSequence;
        captured.kind = event.kind;
        captured.requestedMode = event.requestedMode;
        captured.effectiveMode = event.effectiveMode;
        captured.result = event.result;
        captured.backend = backend;
        captured.requestedUs = event.requestedUs;
        captured.effectiveUs = event.effectiveUs;
        if (captured.kind == ReflexProbeProtocol::FgEventSetOptions ||
            captured.kind == ReflexProbeProtocol::FgEventGetState) {
            captured.fg = event.fg;
            if (captured.kind == ReflexProbeProtocol::FgEventSetOptions)
                UpdateCurrentFgState(captured);
        }
        if (captured.kind == ReflexProbeProtocol::PresentationEventPresent ||
            captured.kind == ReflexProbeProtocol::PresentationEventSwapChain)
            captured.presentation = event.presentation;
        if (captured.kind == ReflexProbeProtocol::ReflexEventSettings)
            UpdateCurrentEffectiveState(captured);
        if (captured.kind == ReflexProbeProtocol::ReflexEventSleep &&
            captured.sequence == 1) {
            wchar_t line[192]{};
            swprintf_s(line, L"Reflex Sleep interception confirmed: %s (first call observed).",
                BackendSleepCallName(captured.backend));
            AppendStatusLineAtQpc(line, captured.qpc);
        }
        HandleCapturedReflexEvent(captured);
        if (rawDebug && rawBatchSuccess)
            rawBatchSuccess = AppendCapturedEventToBuffer(rawBatch, captured, false);
        processedSerial = serial;
    }

    if (processedSerial > g_app.lastEventSerial)
        g_app.lastEventSerial = processedSerial;

    if (rawDebug) {
        if (rawBatchSuccess && rawBatch.length) {
            AppendStatus(rawBatch.data);
        } else if (!rawBatchSuccess && !g_app.rawUiBatchWarningShown) {
            g_app.rawUiBatchWarningShown = true;
            AppendStatusLine(L"WARNING: Live Raw debug UI batching ran out of temporary RAM. The bounded binary Raw debug capture remains active.");
        }

        if (g_app.rawRingWrapNoticePending) {
            g_app.rawRingWrapNoticePending = false;
            AppendStatusLine(L"Raw debug retention reached 131072 calls; oldest retained raw calls are now overwritten as new calls arrive.");
        }
    }
    FreeTextBuffer(rawBatch);

    if (targetExited) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        RecordRunBoundary(qpc.QuadPart);

        DWORD exitCode = 0;
        GetExitCodeProcess(g_app.process, &exitCode);
        wchar_t line[160]{};
        swprintf_s(line, L"Target exited with code %lu.", exitCode);
        AppendStatusLineAtQpc(line, qpc.QuadPart);
        CleanupTarget();
    }
}

} // namespace ReflexProbeController
