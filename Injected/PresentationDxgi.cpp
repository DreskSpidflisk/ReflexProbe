
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

#include "PresentationDxgi.h"

namespace PresentationDxgi {
namespace {

using Factory0 = HRESULT (WINAPI)(REFIID, void**);
using Factory2 = HRESULT (WINAPI)(UINT, REFIID, void**);
using D3D11WithSwapChain = HRESULT (WINAPI)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateChain = HRESULT (STDMETHODCALLTYPE)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwnd = HRESULT (STDMETHODCALLTYPE)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateCore = HRESULT (STDMETHODCALLTYPE)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
using CreateComposition = HRESULT (STDMETHODCALLTYPE)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
using Present = HRESULT (STDMETHODCALLTYPE)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT (STDMETHODCALLTYPE)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using Resize = HRESULT (STDMETHODCALLTYPE)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

ReflexProbeProtocol::SharedState* g_shared = nullptr;
Factory0* g_factory0 = nullptr;
Factory0* g_factory1 = nullptr;
Factory2* g_factory2 = nullptr;
D3D11WithSwapChain* g_d3d11Create = nullptr;

// DXGI proxy factories and application swap chains may have different vtables.
// Preserve each method's true predecessor by *slot*, never by one global
// "original Present" pointer. No allocations on the Present hot path.
struct SlotHook {
    void** slot = nullptr;
    void* previous = nullptr;
};
constexpr size_t kHookCapacity = 128;
SlotHook g_slots[kHookCapacity]{};
SRWLOCK g_slotsLock = SRWLOCK_INIT;
volatile LONG g_missingPresentPredecessorReported = 0;

// Created only when presentation probing is enabled. FLS avoids unconditional
// static TLS in an injected DLL, including runs with all probes unchecked.
// Thread exit frees any lazily allocated cache; no per-Present heap operations.
struct ThreadContext {
    struct CachedOriginal {
        void** slot;
        void* predecessor;
    } cache[8];
    unsigned int factoryDepth;
};
DWORD g_threadContextSlot = FLS_OUT_OF_INDEXES;

VOID CALLBACK FreeThreadContext(PVOID context)
{
    if (context)
        HeapFree(GetProcessHeap(), 0, context);
}

ThreadContext* GetThreadContext()
{
    if (g_threadContextSlot == FLS_OUT_OF_INDEXES)
        return nullptr;
    auto* context = static_cast<ThreadContext*>(FlsGetValue(g_threadContextSlot));
    if (context)
        return context;
    context = static_cast<ThreadContext*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadContext)));
    if (!context)
        return nullptr;
    if (!FlsSetValue(g_threadContextSlot, context)) {
        HeapFree(GetProcessHeap(), 0, context);
        return nullptr;
    }
    return context;
}

// Do not wrap an overlay's DXGI imports: its factory trampoline can call back
// into an IAT entry we replaced, while our saved DXGI export routes into the
// overlay's trampoline. Steam demonstrated precisely such a recursion cycle.
// This intentionally limits discovery to the game executable and its own DLLs.
bool ShouldProbeModule(HMODULE module)
{
    if (!module)
        return false;
    if (module == GetModuleHandleW(nullptr))
        return true;

    wchar_t executable[MAX_PATH]{};
    wchar_t candidate[MAX_PATH]{};
    const DWORD exeChars = GetModuleFileNameW(nullptr, executable, _countof(executable));
    const DWORD moduleChars = GetModuleFileNameW(module, candidate, _countof(candidate));
    if (!exeChars || exeChars >= _countof(executable) ||
        !moduleChars || moduleChars >= _countof(candidate))
        return false;

    const wchar_t* separator = wcsrchr(executable, L'\\');
    if (!separator)
        return false;
    const size_t directoryLength = static_cast<size_t>(separator - executable + 1);
    return _wcsnicmp(executable, candidate, directoryLength) == 0;
}

// A guard is a diagnostic safety net, not a substitute for correct chaining.
volatile LONG g_recursionReported = 0;
struct FactoryScope {
    ThreadContext* context;
    FactoryScope() : context(GetThreadContext()) {
        if (context)
            ++context->factoryDepth;
    }
    ~FactoryScope() {
        if (context)
            --context->factoryDepth;
    }
};

bool RefuseRecursiveFactory()
{
    ThreadContext* context = GetThreadContext();
    // Never forward an unguarded factory trampoline if its per-thread
    // recursion state could not be obtained.
    if (context && context->factoryDepth < 8)
        return false;
    if (g_shared) InterlockedOr(&g_shared->presentationHookBits, 32);
    if (InterlockedCompareExchange(&g_recursionReported, 1, 0) == 0)
        OutputDebugStringW(L"ReflexProbe64: ERROR: recursive DXGI factory forwarding or thread context unavailable; aborting nested factory call.\\n");
    return true;
}

void TraceFactoryImport(HMODULE importer, const char* exportName, void* previous)
{
    wchar_t importerPath[MAX_PATH]{};
    wchar_t ownerPath[MAX_PATH]{};
    GetModuleFileNameW(importer, importerPath, _countof(importerPath));
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(previous), &owner)) {
        GetModuleFileNameW(owner, ownerPath, _countof(ownerPath));
    }
    wchar_t message[2 * MAX_PATH + 160]{};
    swprintf_s(message, L"ReflexProbe64: DXGI IAT patched in %s: %hs, previous=%p in %s\\n",
        importerPath, exportName, previous, ownerPath[0] ? ownerPath : L"(unknown)");
    OutputDebugStringW(message);
}

void* Original(void* instance, size_t index)
{
    if (!instance)
        return nullptr;
    void** vtable = *reinterpret_cast<void***>(instance);
    void** slot = &vtable[index];

    // Only enabled presentation hooks touch this lazy thread cache.
    // Hooked predecessor mappings remain stable once installed.
    ThreadContext* context = GetThreadContext();
    const size_t cacheIndex = index == 8 ? 0 : index == 13 ? 1 :
        index == 22 ? 2 : index == 10 ? 3 : index == 15 ? 4 :
        index == 16 ? 5 : index == 17 ? 6 : 7;
    if (context && context->cache[cacheIndex].slot == slot)
        return context->cache[cacheIndex].predecessor;

    void* result = nullptr;
    AcquireSRWLockShared(&g_slotsLock);
    for (const SlotHook& entry : g_slots) {
        if (entry.slot == slot) {
            result = entry.previous;
            break;
        }
    }
    ReleaseSRWLockShared(&g_slotsLock);
    if (!result && (index == 8 || index == 22)) {
        if (g_shared)
            InterlockedOr(&g_shared->presentationHookBits, 64);
        if (InterlockedCompareExchange(&g_missingPresentPredecessorReported, 1, 0) == 0)
            OutputDebugStringW(L"ReflexProbe64: ERROR: DXGI Present predecessor lookup failed; no safe hook-chain fallback.\n");
    }
    if (result && context) {
        context->cache[cacheIndex].predecessor = result;
        context->cache[cacheIndex].slot = slot;
    }
    return result;
}

bool HookSlot(void* instance, size_t index, void* replacement)
{
    if (!instance)
        return false;
    void** vtable = *reinterpret_cast<void***>(instance);
    void** slot = &vtable[index];
    AcquireSRWLockExclusive(&g_slotsLock);
    for (const SlotHook& entry : g_slots) {
        if (entry.slot == slot) {
            const bool alreadyHooked = *slot == replacement;
            ReleaseSRWLockExclusive(&g_slotsLock);
            return alreadyHooked;
        }
    }
    SlotHook* freeEntry = nullptr;
    for (SlotHook& entry : g_slots) {
        if (!entry.slot) {
            freeEntry = &entry;
            break;
        }
    }
    if (!freeEntry || *slot == replacement) {
        ReleaseSRWLockExclusive(&g_slotsLock);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        ReleaseSRWLockExclusive(&g_slotsLock);
        return false;
    }
    void* previous = *slot;
    // Publish the predecessor mapping before making the replacement visible.
    freeEntry->previous = previous;
    freeEntry->slot = slot;
    MemoryBarrier();
    void* exchanged = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot), replacement, previous);
    if (exchanged != previous) {
        freeEntry->slot = nullptr;
        freeEntry->previous = nullptr;
    }
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    ReleaseSRWLockExclusive(&g_slotsLock);
    return exchanged == previous;
}

void Publish(LONG kind, const ReflexProbeProtocol::PresentationEventData& data, HRESULT result)
{
    if (!g_shared)
        return;
    const LONG count = InterlockedIncrement(&g_shared->presentationCallSerial);
    const LONG serial = InterlockedIncrement(&g_shared->eventSerial);
    auto& event = g_shared->events[static_cast<uint32_t>(serial - 1) %
        ReflexProbeProtocol::kEventCapacity];
    InterlockedExchange(&event.sequence, 0);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.kind = kind;
    event.callSequence = count;
    event.backend = ReflexProbeProtocol::ReflexBackendUnknown;
    event.requestedMode = 0;
    event.effectiveMode = 0;
    event.requestedUs = 0;
    event.effectiveUs = 0;
    event.result = static_cast<LONG>(result);
    event.presentation = data;
    InterlockedExchange(&event.sequence, serial);
}

void PublishChain(IDXGISwapChain* chain, uint32_t method)
{
    if (!g_shared || !chain)
        return;
    ReflexProbeProtocol::PresentationEventData data{};
    data.swapChain = reinterpret_cast<uint64_t>(chain);
    data.method = method;
    DXGI_SWAP_CHAIN_DESC desc{};
    const HRESULT result = chain->GetDesc(&desc);
    if (SUCCEEDED(result)) {
        data.width = desc.BufferDesc.Width;
        data.height = desc.BufferDesc.Height;
        data.buffers = desc.BufferCount;
        data.format = static_cast<uint32_t>(desc.BufferDesc.Format);
        data.swapEffect = static_cast<uint32_t>(desc.SwapEffect);
        data.creationFlags = desc.Flags;
    }
    Publish(ReflexProbeProtocol::PresentationEventSwapChain, data, result);
}

// Present policy reads use acquire semantics; disabled checkboxes preserve both arguments.
constexpr UINT kPresentAllowTearing = 0x200u; // DXGI_PRESENT_ALLOW_TEARING
constexpr UINT kSwapChainAllowTearing = 0x800u; // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
constexpr UINT kPresentTest = 0x1u; // DXGI_PRESENT_TEST

struct ForwardedPresent {
    UINT interval;
    UINT flags;
    uint32_t notes = 0;
};

ForwardedPresent ApplyPresentationPolicy(IDXGISwapChain* chain, UINT interval, UINT flags)
{
    ForwardedPresent forwarded{ interval, flags, 0 };
    if (!g_shared)
        return forwarded;
    const bool overrideSync = ReadAcquire(&g_shared->presentationSyncOverrideEnabled) != 0;
    const bool overrideTearing = ReadAcquire(&g_shared->presentationTearingOverrideEnabled) != 0;
    if (!overrideSync && !overrideTearing)
        return forwarded;
    // Preserve DXGI's non-presenting probe call; never turn an occlusion test into a present.
    if (flags & kPresentTest) {
        forwarded.notes = 16;
        return forwarded;
    }
    if (overrideSync) {
        const LONG selected = ReadAcquire(&g_shared->presentationSyncInterval);
        if (selected >= 0 && selected <= 4)
            forwarded.interval = static_cast<UINT>(selected);
    }
    if (overrideTearing) {
        if (ReadAcquire(&g_shared->presentationTearingOverrideOn) == 0) {
            forwarded.flags &= ~kPresentAllowTearing;
        } else if (forwarded.interval != 0) {
            forwarded.notes = 8;
        } else if ((forwarded.flags & kPresentAllowTearing) == 0) {
            // Only an explicitly selected Force On pays for eligibility checks.
            // Do not change swap-chain creation flags or assume windowed mode.
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(chain->GetDesc(&desc))) {
                forwarded.notes = 4;
            } else if ((desc.Flags & kSwapChainAllowTearing) == 0) {
                forwarded.notes = 1;
            } else {
                BOOL fullscreen = FALSE;
                if (FAILED(chain->GetFullscreenState(&fullscreen, nullptr)))
                    forwarded.notes = 4;
                else if (fullscreen)
                    forwarded.notes = 2;
                else
                    forwarded.flags |= kPresentAllowTearing;
            }
        }
    }
    // A forced positive interval must never forward DXGI_PRESENT_ALLOW_TEARING.
    if (forwarded.interval != 0)
        forwarded.flags &= ~kPresentAllowTearing;
    return forwarded;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT interval, UINT flags)
{
    Present* real = reinterpret_cast<Present*>(Original(chain, 8));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const ForwardedPresent forwarded = ApplyPresentationPolicy(chain, interval, flags);
    const HRESULT result = real(chain, forwarded.interval, forwarded.flags);
    ReflexProbeProtocol::PresentationEventData data{};
    data.swapChain = reinterpret_cast<uint64_t>(chain);
    data.method = 0;
    data.syncInterval = interval;
    data.presentFlags = flags;
    data.forwardedSyncInterval = forwarded.interval;
    data.forwardedPresentFlags = forwarded.flags;
    data.overrideNotes = forwarded.notes;
    Publish(ReflexProbeProtocol::PresentationEventPresent, data, result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookPresent1(
    IDXGISwapChain1* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    Present1* real = reinterpret_cast<Present1*>(Original(chain, 22));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const ForwardedPresent forwarded = ApplyPresentationPolicy(chain, interval, flags);
    const HRESULT result = real(chain, forwarded.interval, forwarded.flags, parameters);
    ReflexProbeProtocol::PresentationEventData data{};
    data.swapChain = reinterpret_cast<uint64_t>(chain);
    data.method = 1;
    data.syncInterval = interval;
    data.presentFlags = flags;
    data.forwardedSyncInterval = forwarded.interval;
    data.forwardedPresentFlags = forwarded.flags;
    data.overrideNotes = forwarded.notes;
    Publish(ReflexProbeProtocol::PresentationEventPresent, data, result);
    return result;
}

HRESULT STDMETHODCALLTYPE HookResize(
    IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    Resize* real = reinterpret_cast<Resize*>(Original(chain, 13));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = real(chain, count, width, height, format, flags);
    if (SUCCEEDED(result))
        PublishChain(chain, 7);
    return result;
}

void HookChain(IDXGISwapChain* chain, uint32_t origin)
{
    if (!chain)
        return;
    // Record the chain before mutating any vtable entry.
    PublishChain(chain, origin);
    if (HookSlot(chain, 8, reinterpret_cast<void*>(&HookPresent)) && g_shared)
        InterlockedOr(&g_shared->presentationHookBits, 2);
    if (HookSlot(chain, 13, reinterpret_cast<void*>(&HookResize)) && g_shared)
        InterlockedOr(&g_shared->presentationHookBits, 8);
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain1),
            reinterpret_cast<void**>(&chain1))) && chain1) {
        if (HookSlot(chain1, 22, reinterpret_cast<void*>(&HookPresent1)) && g_shared)
            InterlockedOr(&g_shared->presentationHookBits, 4);
        chain1->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookCreateChain(
    IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    CreateChain* real = reinterpret_cast<CreateChain*>(Original(factory, 10));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = real(factory, device, desc, out);
    if (SUCCEEDED(result) && out && *out)
        HookChain(*out, 2);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateHwnd(
    IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
    IDXGISwapChain1** out)
{
    CreateHwnd* real = reinterpret_cast<CreateHwnd*>(Original(factory, 15));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = real(factory, device, hwnd, desc, fullscreen, output, out);
    if (SUCCEEDED(result) && out && *out)
        HookChain(*out, 3);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateCore(
    IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out)
{
    CreateCore* real = reinterpret_cast<CreateCore*>(Original(factory, 16));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = real(factory, device, window, desc, output, out);
    if (SUCCEEDED(result) && out && *out)
        HookChain(*out, 4);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateComposition(
    IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* output, IDXGISwapChain1** out)
{
    CreateComposition* real = reinterpret_cast<CreateComposition*>(Original(factory, 17));
    if (!real)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = real(factory, device, desc, output, out);
    if (SUCCEEDED(result) && out && *out)
        HookChain(*out, 5);
    return result;
}

void HookFactory(IUnknown* factory)
{
    if (!factory)
        return;
    bool patched = false;
    IDXGIFactory* factory0 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory),
            reinterpret_cast<void**>(&factory0))) && factory0) {
        const bool hooked = HookSlot(factory0, 10, reinterpret_cast<void*>(&HookCreateChain));
        patched = hooked || patched;
        factory0->Release();
    }
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(&factory2))) && factory2) {
        const bool hwnd = HookSlot(factory2, 15, reinterpret_cast<void*>(&HookCreateHwnd));
        const bool core = HookSlot(factory2, 16, reinterpret_cast<void*>(&HookCreateCore));
        const bool composition = HookSlot(factory2, 17,
            reinterpret_cast<void*>(&HookCreateComposition));
        patched = hwnd || core || composition || patched;
        factory2->Release();
    }
    if (patched && g_shared)
        InterlockedOr(&g_shared->presentationHookBits, 1);
}

HRESULT WINAPI HookFactory0(REFIID iid, void** out)
{
    if (RefuseRecursiveFactory())
        return E_FAIL;
    FactoryScope scope;
    if (!g_factory0)
        return E_FAIL;
    const HRESULT result = g_factory0(iid, out);
    if (SUCCEEDED(result) && out && *out)
        HookFactory(reinterpret_cast<IUnknown*>(*out));
    return result;
}
HRESULT WINAPI HookFactory1(REFIID iid, void** out)
{
    if (RefuseRecursiveFactory())
        return E_FAIL;
    FactoryScope scope;
    if (!g_factory1)
        return E_FAIL;
    const HRESULT result = g_factory1(iid, out);
    if (SUCCEEDED(result) && out && *out)
        HookFactory(reinterpret_cast<IUnknown*>(*out));
    return result;
}
HRESULT WINAPI HookFactory2(UINT flags, REFIID iid, void** out)
{
    if (RefuseRecursiveFactory())
        return E_FAIL;
    FactoryScope scope;
    if (!g_factory2)
        return E_FAIL;
    const HRESULT result = g_factory2(flags, iid, out);
    if (SUCCEEDED(result) && out && *out)
        HookFactory(reinterpret_cast<IUnknown*>(*out));
    return result;
}

HRESULT WINAPI HookD3D11WithSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* chosen, ID3D11DeviceContext** context)
{
    if (!g_d3d11Create)
        return E_FAIL;
    const HRESULT result = g_d3d11Create(adapter, driver, software, flags,
        levels, levelCount, sdkVersion, desc, chain, device, chosen, context);
    if (SUCCEEDED(result) && chain && *chain)
        HookChain(*chain, 6);
    if (g_shared)
        InterlockedOr(&g_shared->presentationHookBits, 16);
    return result;
}

// Only canonical exports are patched. Existing overlays / hooks are not replaced.
struct ExportHook {
    const char* name;
    FARPROC target;
    void* hook;
};

// Share the exact export-name vocabulary with the GetProcAddress early filter.
constexpr const char* kDynamicExports[] = {
    "CreateDXGIFactory", "CreateDXGIFactory1",
    "CreateDXGIFactory2", "D3D11CreateDeviceAndSwapChain"
};

size_t Exports(HMODULE module, ExportHook* hooks, size_t capacity)
{
    if (!module || capacity < 4)
        return 0;
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(module, path, MAX_PATH))
        return 0;
    const char* basename = strrchr(path, '\\');
    basename = basename ? basename + 1 : path;
    size_t count = 0;
    if (_stricmp(basename, "dxgi.dll") == 0) {
        if (!g_factory0)
            g_factory0 = reinterpret_cast<Factory0*>(GetProcAddress(module, "CreateDXGIFactory"));
        if (!g_factory1)
            g_factory1 = reinterpret_cast<Factory0*>(GetProcAddress(module, "CreateDXGIFactory1"));
        if (!g_factory2)
            g_factory2 = reinterpret_cast<Factory2*>(GetProcAddress(module, "CreateDXGIFactory2"));
        hooks[count++] = {kDynamicExports[0], reinterpret_cast<FARPROC>(g_factory0), reinterpret_cast<void*>(&HookFactory0)};
        hooks[count++] = {kDynamicExports[1], reinterpret_cast<FARPROC>(g_factory1), reinterpret_cast<void*>(&HookFactory1)};
        hooks[count++] = {kDynamicExports[2], reinterpret_cast<FARPROC>(g_factory2), reinterpret_cast<void*>(&HookFactory2)};
    } else if (_stricmp(basename, "d3d11.dll") == 0) {
        if (!g_d3d11Create)
            g_d3d11Create = reinterpret_cast<D3D11WithSwapChain*>(
                GetProcAddress(module, "D3D11CreateDeviceAndSwapChain"));
        hooks[count++] = {kDynamicExports[3],
            reinterpret_cast<FARPROC>(g_d3d11Create), reinterpret_cast<void*>(&HookD3D11WithSwapChain)};
    }
    return count;
}

} // namespace

bool IsDynamicExportName(const char* name)
{
    if (!name)
        return false;
    for (const char* candidate : kDynamicExports)
        if (strcmp(name, candidate) == 0)
            return true;
    return false;
}

bool Initialize(ReflexProbeProtocol::SharedState* shared)
{
    // Do not allocate an FLS slot or touch thread caches unless this probe
    // was explicitly enabled during acquisition.
    const DWORD slot = FlsAlloc(&FreeThreadContext);
    if (slot == FLS_OUT_OF_INDEXES)
        return false;
    g_threadContextSlot = slot;
    g_shared = shared;
    return true;
}

bool IsApplicationModule(HMODULE module)
{
    return ShouldProbeModule(module);
}

bool IsApplicationCaller(const void* address)
{
    if (!address)
        return false;
    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &callerModule))
        return false;
    return ShouldProbeModule(callerModule);
}

FARPROC ResolveDynamic(HMODULE module, LPCSTR name, FARPROC original)
{
    if (!g_shared || !module || !name || !original)
        return original;
    ExportHook exports[4]{};
    const size_t count = Exports(module, exports, _countof(exports));
    for (size_t i = 0; i < count; ++i) {
        if (exports[i].target == original && strcmp(exports[i].name, name) == 0)
            return reinterpret_cast<FARPROC>(exports[i].hook);
    }
    return original;
}

bool PatchImports(HMODULE module)
{
    if (!g_shared || !ShouldProbeModule(module))
        return true;
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return true;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return true;
    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return true;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;
        const char* name = reinterpret_cast<const char*>(base + descriptor->Name);
        HMODULE provider = nullptr;
        if (_stricmp(name, "dxgi.dll") == 0)
            provider = GetModuleHandleW(L"dxgi.dll");
        else if (_stricmp(name, "d3d11.dll") == 0)
            provider = GetModuleHandleW(L"d3d11.dll");
        if (!provider)
            continue;
        ExportHook exports[4]{};
        const size_t count = Exports(provider, exports, _countof(exports));
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            void* current = reinterpret_cast<void*>(
                static_cast<uintptr_t>(thunk->u1.Function));
            for (size_t i = 0; i < count; ++i) {
                if (!exports[i].target || current != reinterpret_cast<void*>(exports[i].target))
                    continue;
                DWORD oldProtection = 0;
                if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                        PAGE_READWRITE, &oldProtection))
                    return false;
                const void* prior = InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(&thunk->u1.Function),
                    exports[i].hook, current);
                DWORD ignored = 0;
                VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                    oldProtection, &ignored);
                if (prior == current)
                    TraceFactoryImport(module, exports[i].name, current);
                break;
            }
        }
    }
    return true;
}

} // namespace PresentationDxgi
