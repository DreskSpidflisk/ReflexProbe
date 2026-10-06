#include "ControllerInternal.h"

#include <windowsx.h>
#include <wchar.h>
#include <stdio.h>

namespace ReflexProbeController {

void ResetLiveStateTracking()
{
    g_app.haveStateEvent = false;
    g_app.stateEvent = {};
    g_app.stateRepeatCount = 0;
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
    default:
        return L"slReflexSetOptions";
    }
}

CaptureMode CurrentCaptureMode()
{
    return g_app.captureMode;
}

void FormatReflexEvent(const CapturedReflexEvent& event, wchar_t* line, size_t lineCount)
{
    const wchar_t* mode = event.mode == 2 ? L"On + Boost" :
                          (event.mode == 1 ? L"On" : L"Off");
    const wchar_t* callName = BackendCallName(event.backend);

    if (event.requestedUs && event.effectiveUs) {
        swprintf_s(line, lineCount,
            L"%s #%ld: mode=%s, requested=%u us (%.3f FPS), effective=%u us (%.3f FPS), result=%ld",
            callName, event.sequence, mode,
            event.requestedUs, 1000000.0 / static_cast<double>(event.requestedUs),
            event.effectiveUs, 1000000.0 / static_cast<double>(event.effectiveUs),
            event.result);
    } else if (event.requestedUs) {
        swprintf_s(line, lineCount,
            L"%s #%ld: mode=%s, requested=%u us (%.3f FPS), effective=0 us (no explicit frame limit), result=%ld",
            callName, event.sequence, mode,
            event.requestedUs, 1000000.0 / static_cast<double>(event.requestedUs), event.result);
    } else if (event.effectiveUs) {
        swprintf_s(line, lineCount,
            L"%s #%ld: mode=%s, requested=0 us (automatic), effective=%u us (%.3f FPS), result=%ld",
            callName, event.sequence, mode,
            event.effectiveUs, 1000000.0 / static_cast<double>(event.effectiveUs), event.result);
    } else {
        swprintf_s(line, lineCount,
            L"%s #%ld: mode=%s, requested=0 us (automatic), effective=0 us (no explicit frame limit), result=%ld",
            callName, event.sequence, mode, event.result);
    }
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

void AppendReflexEvent(const CapturedReflexEvent& event, bool newState)
{
    wchar_t line[512]{};
    FormatReflexDisplayLine(event, newState, line, _countof(line));
    AppendDisplayLineAtQpc(line, event.qpc);
}

bool AppendReflexEventToBuffer(TextBuffer& buffer, const CapturedReflexEvent& event, bool newState)
{
    wchar_t line[512]{};
    FormatReflexDisplayLine(event, newState, line, _countof(line));
    return AppendTextBufferLineAtQpc(buffer, line, event.qpc);
}

bool SameReflexState(const CapturedReflexEvent& a, const CapturedReflexEvent& b)
{
    return a.backend == b.backend &&
           a.mode == b.mode &&
           a.requestedUs == b.requestedUs &&
           a.effectiveUs == b.effectiveUs &&
           a.result == b.result;
}

void FormatRepeatCount(uint64_t repeatCount, wchar_t* line, size_t lineCount)
{
    swprintf_s(line, lineCount, L"Previous Reflex State repeated %llu more times.",
        static_cast<unsigned long long>(repeatCount));
}

void AppendRepeatCount(uint64_t repeatCount, LONGLONG eventQpc)
{
    if (!repeatCount)
        return;

    wchar_t line[128]{};
    FormatRepeatCount(repeatCount, line, _countof(line));
    AppendDisplayLineAtQpc(line, eventQpc);
}

void FinalizeStateRun(LONGLONG eventQpc)
{
    if (g_app.haveStateEvent)
        AppendRepeatCount(g_app.stateRepeatCount, eventQpc);
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

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    const CaptureMode previous = g_app.captureMode;
    if (previous == CaptureModeStateChanges)
        FinalizeStateRun(qpc.QuadPart);
    else
        ResetLiveStateTracking();

    if (mode == CaptureModeRawDebug) {
        if (!EnsureRawDebugBuffer()) {
            MessageBoxW(g_app.window,
                L"Could not allocate the bounded Raw debug capture buffer.",
                L"ReflexProbe", MB_ICONERROR);
            g_app.captureMode = previous;
            SetCaptureButtons(previous);
            return false;
        }
        ResetRawDebugCapture();
    }

    g_app.captureMode = mode;
    SetCaptureButtons(mode);

    if (mode == CaptureModeRawDebug) {
        AppendStatusLineAtQpc(
            L"Capture mode changed to Raw debug. Raw retention starts now; earlier calls are not reconstructed.",
            qpc.QuadPart);
    } else {
        wchar_t line[224]{};
        swprintf_s(line,
            L"Capture mode changed to State changes. Raw debug stopped after %llu captured call(s); state tracking resumes with the next Reflex call.",
            static_cast<unsigned long long>(g_app.rawDebugTotalCaptured));
        AppendStatusLineAtQpc(line, qpc.QuadPart);
    }

    return true;
}

void HandleCapturedReflexEvent(const CapturedReflexEvent& event)
{
    if (CurrentCaptureMode() == CaptureModeRawDebug) {
        CaptureRawDebugEvent(event);
        return;
    }

    if (!g_app.haveStateEvent) {
        g_app.haveStateEvent = true;
        g_app.stateEvent = event;
        g_app.stateRepeatCount = 0;
        AppendReflexEvent(event, true);
    } else if (SameReflexState(g_app.stateEvent, event)) {
        ++g_app.stateRepeatCount;
    } else {
        AppendRepeatCount(g_app.stateRepeatCount, event.qpc);
        AppendReflexEvent(event, true);
        g_app.stateEvent = event;
        g_app.stateRepeatCount = 0;
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
        g_app.lastHookState = hookState;
        wchar_t line[2300]{};
        switch (hookState) {
        case ReflexProbeProtocol::HookStateWaitingForDll:
            wcscpy_s(line, L"Waiting for a Streamline Reflex interception path...");
            break;
        case ReflexProbeProtocol::HookStateInterceptArmed:
            swprintf_s(line, L"Interception armed: %s. Waiting for sl.reflex.dll to resolve its plugin gateway.",
                BackendName(backend));
            break;
        case ReflexProbeProtocol::HookStateReflexFound:
            swprintf_s(line, L"Found Reflex backend: %s\r\nModule: %s\r\nFile version: %s",
                BackendName(backend),
                g_app.shared->reflexPath[0] ? g_app.shared->reflexPath : L"(path unavailable)",
                g_app.shared->reflexVersion[0] ? g_app.shared->reflexVersion : L"unknown");
            break;
        case ReflexProbeProtocol::HookStateHooked:
            swprintf_s(line, L"Hook active: %s via %s\r\nModule: %s\r\nFile version: %s",
                BackendCallName(backend), BackendName(backend),
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
            L"WARNING: Reflex transport ring overrun; %ld call(s) were lost before the controller drained them. Capture continuity is broken at this point.",
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
        captured.sequence = serial;
        captured.mode = event.mode;
        captured.result = event.result;
        captured.backend = backend;
        captured.requestedUs = event.requestedUs;
        captured.effectiveUs = event.effectiveUs;
        HandleCapturedReflexEvent(captured);
        if (rawDebug && rawBatchSuccess)
            rawBatchSuccess = AppendReflexEventToBuffer(rawBatch, captured, false);
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
