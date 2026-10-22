#include "ControllerInternal.h"
#include <stdio.h>

namespace ReflexProbeController {

const wchar_t* PresentationOriginName(uint32_t method)
{
    switch (method) {
    case 2: return L"CreateSwapChain";
    case 3: return L"CreateSwapChainForHwnd";
    case 4: return L"CreateSwapChainForCoreWindow";
    case 5: return L"CreateSwapChainForComposition";
    case 6: return L"D3D11CreateDeviceAndSwapChain";
    case 7: return L"ResizeBuffers";
    default: return L"unknown";
    }
}

void FormatPresentationEvent(const CapturedReflexEvent& event, wchar_t* line, size_t count)
{
    const auto& p = event.presentation;
    if (event.kind == ReflexProbeProtocol::PresentationEventSwapChain) {
        swprintf_s(line, count,
            L"DXGI swap chain 0x%llX: %s, %ux%u, %u buffers, format=%u, swapEffect=%u, flags=0x%X, GetDesc=0x%08X",
            static_cast<unsigned long long>(p.swapChain), PresentationOriginName(p.method),
            p.width, p.height, p.buffers, p.format, p.swapEffect,
            p.creationFlags, static_cast<unsigned int>(event.result));
    } else {
        wchar_t sync[64]{};
        wchar_t flags[128]{};
        if (p.syncInterval == p.forwardedSyncInterval)
            swprintf_s(sync, L"SyncInterval=%u", p.syncInterval);
        else
            swprintf_s(sync, L"SyncInterval=%u -> %u", p.syncInterval, p.forwardedSyncInterval);
        if (p.presentFlags == p.forwardedPresentFlags)
            swprintf_s(flags, L"Flags=0x%X%s", p.presentFlags,
                (p.presentFlags & 0x200u) ? L" (ALLOW_TEARING)" : L"");
        else
            swprintf_s(flags, L"Flags=0x%X%s -> 0x%X%s", p.presentFlags,
                (p.presentFlags & 0x200u) ? L" (ALLOW_TEARING)" : L"",
                p.forwardedPresentFlags,
                (p.forwardedPresentFlags & 0x200u) ? L" (ALLOW_TEARING)" : L"");
        const wchar_t* notes = p.overrideNotes == 1 ? L", tearing On skipped: chain lacks ALLOW_TEARING" :
            p.overrideNotes == 2 ? L", tearing On skipped: exclusive fullscreen" :
            p.overrideNotes == 4 ? L", tearing On skipped: chain state unavailable" :
            p.overrideNotes == 8 ? L", tearing On suppressed: SyncInterval > 0" :
            p.overrideNotes == 16 ? L", overrides skipped: DXGI_PRESENT_TEST" : L"";
        swprintf_s(line, count,
            L"DXGI %s #%ld: chain=0x%llX, %s, %s%s, HRESULT=0x%08X",
            p.method == 1 ? L"Present1" : L"Present", event.sequence,
            static_cast<unsigned long long>(p.swapChain), sync, flags, notes,
            static_cast<unsigned int>(event.result));
    }
}

void AppendPresentationRepeat(const PresentationStateRun& run, LONGLONG qpc)
{
    if (!run.repeats)
        return;
    wchar_t line[224]{};
    swprintf_s(line, L"PREVIOUS DXGI %s: %llu repeated calls for chain=0x%llX since #%ld.",
        run.method == 1 ? L"Present1" : L"Present",
        static_cast<unsigned long long>(run.repeats),
        static_cast<unsigned long long>(run.swapChain), run.state.sequence);
    AppendDisplayLineAtQpc(line, qpc);
}

void HandlePresentationEvent(const CapturedReflexEvent& event)
{
    if (event.kind == ReflexProbeProtocol::PresentationEventSwapChain) {
        // A resized or replaced chain starts a new visible presentation interval.
        for (auto& run : g_app.presentationRuns) {
            if (run.used && run.swapChain == event.presentation.swapChain) {
                AppendPresentationRepeat(run, event.qpc);
                run = {};
            }
        }
        wchar_t line[480]{};
        FormatPresentationEvent(event, line, _countof(line));
        AppendDisplayLineAtQpc(line, event.qpc);
        return;
    }
    const auto& p = event.presentation;
    PresentationStateRun* stream = nullptr;
    for (auto& candidate : g_app.presentationRuns) {
        if (candidate.used && candidate.swapChain == p.swapChain && candidate.method == p.method) {
            stream = &candidate;
            break;
        }
        if (!candidate.used && !stream)
            stream = &candidate;
    }
    if (!stream) {
        if (!g_app.presentationTrackingOverflowWarned) {
            g_app.presentationTrackingOverflowWarned = true;
            AppendStatusLineAtQpc(L"WARNING: DXGI tracking table full; additional swap-chain streams omitted in State changes mode.", event.qpc);
        }
        return;
    }
    if (stream->used) {
        const auto& previous = stream->state;
        if (previous.presentation.syncInterval == p.syncInterval &&
            previous.presentation.presentFlags == p.presentFlags &&
            previous.presentation.forwardedSyncInterval == p.forwardedSyncInterval &&
            previous.presentation.forwardedPresentFlags == p.forwardedPresentFlags &&
            previous.presentation.overrideNotes == p.overrideNotes &&
            previous.result == event.result) {
            ++stream->repeats;
            return;
        }
        AppendPresentationRepeat(*stream, event.qpc);
    }
    stream->used = true;
    stream->swapChain = p.swapChain;
    stream->method = p.method;
    stream->state = event;
    stream->repeats = 0;
    wchar_t details[480]{};
    FormatPresentationEvent(event, details, _countof(details));
    wchar_t line[512]{};
    swprintf_s(line, L"NEW %s", details);
    AppendDisplayLineAtQpc(line, event.qpc);
}


} // namespace ReflexProbeController
