# ReflexProbe — Architecture and Non-Regression Invariants

> Persistent engineering reference for ReflexProbe's Windows x64 controller and injected DLL. Reflects the October 22, 2026 experimental `experiment/presentation-probing` branch. Not a claim that all paths are build- or runtime-verified. Read current source and `docs/TestFindings.md` alongside it.

## Reference documents and source map

- `README.md`: user-visible features, defaults and build instructions. `docs/TestFindings.md`: empirical, per-game observations that must **not be retroactively rewritten** when instrumentation changes.
- `Common/Protocol.h`: versioned POD shared-memory policy/events, build tag and 4,096-event transport ring. Change protocol version whenever the layout changes; rebuild controller and DLL together.
- `Controller/ReflexProbe.cpp` and `Controller/ControllerInternal.h`: native Win32 user interface (BUTTON/CHECKBOX/RADIOBUTTON/COMBOBOX, standard font, measured widths, `MoveWindow`, `WM_COMMAND`). This is **not** a browser or custom graphics toolkit.
- `Controller/Process.cpp`: Launch / exact-path Watch / Attach, initial policy, injection and startup diagnostics.
- `Controller/Capture.cpp`: Reflex and FG events, state-change compression, optional Sleep accounting, Raw debug. `Controller/PresentationCapture.cpp`: DXGI formatting and per-swap-chain Present comparisons. `Controller/Status.cpp`: bounded Win32 EDIT display.
- `Injected/ReflexProbe64.cpp`: module discovery, Reflex/Streamline/NVAPI/FG function-pointer substitution, overrides and event publication. `Injected/PresentationDxgi.cpp`: independently enabled DXGI factory/swap-chain observation and Present overrides.

## Feature gates and non-regression invariants

**Golden rule: shipping a probe does not activate it. With its checkbox unchecked, it must not install that probe's interception or alter another selected probe's discovery, backend ownership, or runtime behavior. Additional enabled probes must not implicitly expand an already-established Reflex interception path.**

1. **Reflex unchecked:** no Reflex setter interception, no SL1 or native NVAPI Reflex pointer substitution, no Reflex Sleep interception.
2. **DLSS FG unchecked:** no FG SetOptions or GetState wrapper and no FG overrides. With FG checked but **GetState unchecked**, the genuine `slDLSSGGetState` pointer is returned untouched.
3. **DXGI Presentation unchecked:** no DXGI factory, swap-chain, Present, Present1 or ResizeBuffers hook; no presentation overrides. Compiled code and visible controls alone do not change a target.
4. **Presentation only:** shared GetProcAddress machinery may inspect selected game-local DXGI exports, but **must not substitute `slGetFeatureFunction`, the SL1 plugin gateway, Reflex/NVAPI setters, Sleep or FG functions**. Resolver plumbing is shared; feature authorization is not.
5. **Combined probes:** a Presentation-enabled worker can keep discovering DXGI modules after a Streamline Reflex path is selected. This extended worker lifetime **must not install an additional native NVAPI Reflex interceptor** because DXGI is enabled. FG may independently need later resolver discovery; do not terminate its legitimate search prematurely.
6. **Acquisition-time decisions:** the three probe selections, Reflex Sleep wrapping, and FG GetState wrapping are fixed for a target. **Live policies:** permitted Reflex/FG/DXGI override values/enable checkboxes can be changed for subsequent intercepted calls. All override checkboxes start unchecked; unchecked means game value forwarded.
7. **No unrequested side effects:** no capture files, no overlay, no swap-chain recreation, no editing application data, no unrelated API wrapper for an unchecked feature. Use RAM for diagnostic retention.

## Reflex path ownership

| Backend | Existing hook and compatibility fact |
| --- | --- |
| Modern Streamline 2.x+ | `slGetFeatureFunction` from direct IAT or game-module `GetProcAddress`, then `slReflexSetOptions` when Reflex selected and optional `slReflexSleep` when Sleep counting selected. Resolver can be in a game DLL, not just the EXE (Satisfactory). |
| Legacy Streamline 1.x | Primary plugin gateway: `sl.reflex.dll!slGetPluginFunction("slSetConstants")` through the interposer. Older public `slSetFeatureConstants` Reflex ID 3 is fallback. **SL1 Sleep counting is not implemented.** Requiem uses the plugin path. |
| Native NVAPI D3D | Game-facing `nvapi_QueryInterface` / `NvAPI_D3D_SetSleepMode` function seam, with optional `NvAPI_D3D_Sleep`; no patching code bytes in `nvapi64.dll`. **God of War (2018) and Shadow Warrior 3 are confirmed native NVAPI users.** |

Resolver discovery must allow late module loading: SL1 may load its interposer after injection, and modern Streamline can resolve in another game DLL. Main-EXE-only scanning would break already-confirmed titles. Watch should target the actual x64 executable, not a bootstrap wrapper.

### October 22 regression: DXGI changed discovery lifetime

- Pre-DXGI Reflex-only code exited its worker after a Streamline import was found when FG probing was off. The *first* experimental DXGI commit added an additional requirement that Presentation probing be off, thereby keeping Reflex discovery alive in Reflex+Presentation sessions.
- Requiem then produced alternating apparent `6329` and `0 -> 6329` microsecond events, all labelled native NVAPI despite a successfully intercepted SL1 plugin gateway. This demonstrates a logging/attribution problem and strong evidence of overlapping interception; it does **not** prove the calls were nested on one thread or that GPU pacing oscillated.
- Pre-existing weakness: `SharedState::backend` is **mutable discovery/status information**; the original event records had no backend field, and the controller relabelled buffered events using the last global backend. Fix: publish backend identity **per Reflex event** at the originating hook, including optional Sleep events.
- Worker isolation fix: select the Streamline Reflex boundary independently from Presentation's module-search lifetime, and stop later acquisition of native NVAPI simply because Presentation remains active.
- **Residual uncertainty:** NVAPI could have been hooked *before* Streamline was selected. Do not conceal genuine game requests by removing requested-value equality or globally dropping native calls. Verify actual call ownership if overlapping events persist.

## Event and UI invariants

- Each Reflex settings event records originating backend, requested mode/interval, forwarded mode/interval, API result, and settings-call number. Sleep has a separate sequence. Per-event backend determines formatting and SL1 Sleep capability, **not** latest global backend.
- `SameReflexState` deliberately compares requested **and** forwarded values plus backend/result. A genuine `0 -> 6329` transition is meaningful even if the override keeps forwarded 6329. Fix duplicate source events instead of weakening this comparison.
- State-change mode collapses identical logical calls; Raw debug retains at most 131,072 calls **in RAM**, no disk writes, and is meant for focused diagnostics. The Win32 EDIT log is bounded and batched; transport gaps are reported rather than hidden.
- FG SetOptions/GetState histories are distinct, and transient GetState frame-count fields are excluded from persistent settings equivalence. DXGI Present/Present1 states aggregate by swap chain and method.
- `frameLimitUs=0` is no explicit Reflex frame limit, **not** Reflex Off. A successful intercepted setter is not proof of observed framerate or functioning FG.
- UI is native Win32: checkboxes, radios, combo boxes, editable FPS field, measured inline widths, explicit group spacing, and `WM_COMMAND`; inspect the latest supplied screenshot and `LayoutControls` before UI modifications. Feature selections lock for the current target; live overrides remain available when their family is selected.

## DXGI Presentation experiment

- Enabled only by its own checkbox. Swap-chain factory and vtable hooks are scoped to game-local modules and retain predecessor mapping and recursion guards; earlier Steam overlay recursion and stack-overflow findings make these constraints non-negotiable.
- Independent unchecked-by-default live overrides: SyncInterval choices **0–4** (selected 0 initially); ALLOW_TEARING **Off/On** radios (Off selected initially). Unchecked is Game Default; do not add a redundant Game Default dropdown entry.
- Present/Present1 log original and forwarded SyncInterval/flags plus HRESULT. Only edit `DXGI_PRESENT_ALLOW_TEARING` (0x200) in flags, preserving unrelated bits. Forcing nonzero SyncInterval clears incompatible tearing bit.
- Force tearing On only for appropriate nonexclusive chains created with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` (0x800); otherwise log skip, never rewrite creation flags. Preserve DXGI_PRESENT_TEST calls.
- `ALLOW_TEARING` grants permission, **not proof of physical tearing**; driver-enforced VSync/G-SYNC or other pacing may still apply. Present timestamps are captured after call return rather than at scanout.
- No Vulkan Present mode or Vulkan Reflex extension interception is implemented by DXGI hooks.

## Compatibility anchors from TestFindings

| Game | Must preserve |
| --- | --- |
| God of War (2018), GOG / D3D11 | Native NVAPI sparse setters; Force Boost, override and optional Sleep. Working explicit limit with Reflex Off and zero observed Sleep. |
| Shadow Warrior 3, GOG / D3D11 | Native NVAPI; exact-path Watch of inner Win64 binary; sparse setters, explicit limit and optional Sleep. |
| A Plague Tale: Requiem, GOG / D3D12 | SL1 private plugin hook and zero original intervals; no SL1 Sleep capture. Its separately observed forced-Boost pacing collapse measured with PresentMon remains a historical finding. |
| Satisfactory, Steam / D3D12 | Modern resolver from `FactoryGameSteam-Core-Win64-Shipping.dll`, not just main EXE. |
| DOOM: The Dark Ages, Steam / Vulkan | Modern Streamline explicit limit and Watch behavior, independent of DXGI interception. |

## Review, changes, and verification

- Check disabled, individually enabled and combined probe paths before changes. Respect untouched optional Sleep/GetState function pointers and avoid changing Win32 layout as part of an interception fix.
- Keep discovery ownership, event schema/protocol, feature gates and documentation in separate meaningful commits. Production `main` remains untouched without separate authorization.
- Protocol changes require building both matching controller and injected DLL; build identity includes tag, protocol, timestamps, binary hashes. **Source review is not a successful compile; compile is not a game-runtime test.**
- Targeted verification *when available*: Requiem with Reflex+Presentation, one native NVAPI title, one modern Streamline title, plus Presentation-only gate. Do **not** demand full-game-library regression testing for an isolated fix.
- Only add actual measurements to `docs/TestFindings.md`. Keep uncertainty explicit and separate code invariants from empirical results.
