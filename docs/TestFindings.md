# ReflexProbe — Test Findings

This document records the runtime observations, integration differences, and acquisition/regression findings behind [ReflexProbe](../README.md). It is an **empirical test record** for specific game builds and driver conditions, not a statement about every version of those games or undocumented NVIDIA internals. Unless stated otherwise, observed requested/effective fields are taken from intercepted Reflex calls; frame-rate observations and inferred FG relationships are separate evidence.

**Terminology:** `frameLimitUs=0` means *no explicit Reflex frame-limit interval*, not “Reflex disabled” and not a request for any particular implicit cap. With an active override, “requested” is the game's original setting and “effective” is the value passed onward after ReflexProbe policy. `6329 µs` targets approximately **158.003 FPS**.

**Contents:** [Cross-title results](#cross-title-results) · [Per-game results](#per-game-results) · [Resolver discovery and regression](#resolver-discovery-and-regression) · [Instrumentation and maintenance notes](#instrumentation-and-maintenance-notes)

## Cross-title results

### Automatic below-refresh behavior

Across observed D3D Reflex + VSync + G-SYNC cases, the automatic cap is very well described by adding roughly `1/3600` second of frame-time headroom to the refresh interval:

```text
cap(R) ~= 1 / ((1 / R) + (1 / 3600))
       ~= 3600R / (3600 + R)
```

Examples:

```text
120 Hz -> ~116.13 FPS
144 Hz -> ~138.46 FPS
165 Hz -> ~157.77 FPS
240 Hz -> 225.00 FPS
360 Hz -> ~327.27 FPS
```

This is an empirically reconstructed operational rule, not a claim about unpublished NVIDIA driver source.

The pattern above comes from an observed Reflex + VSync + G-SYNC configuration, especially at 240 Hz. It is **not** a claim that Vulkan lacks explicit Reflex limiting: DOOM: The Dark Ages demonstrates working explicit limits on Vulkan. Conversely, No Man's Sky demonstrates that accepted explicit settings do not always govern output.

### Explicit limit versus Reflex Low Latency

The Reflex frame limiter and the Reflex Low Latency mode are distinct settings: modern Streamline uses `sl::ReflexOptions::frameLimitUs`; legacy Streamline 1.x used `sl::ReflexConstants::frameLimitUs`; native D3D NVAPI uses `minimumIntervalUs`. A nonzero interval can remain active while the Reflex mode is Off.

Controlled GSyncProbe overrides at **165, 60, and 30 FPS** changed measured App Present Rate, including when Low Latency mode was Off. Most sampled commercial games submitted zero explicit interval; **Satisfactory** is the first in this test set observed intentionally submitting a nonzero Reflex limit of **16,666 µs (~60.002 FPS)** for its main menu. DOOM provides a console variable that maps literally to the requested field.

A game's own separate FPS limiter can coexist with the Reflex limit. In Hogwarts Legacy, reducing the game-defined limit below the forced Reflex ceiling reduced FPS further without any intercepted Reflex state change. Indiana Jones and No Man's Sky also expose separate pre-FG limiters.

### Sleep counting and rendered-frame cadence

| Specimen | Observed application-level Sleep behavior | Interpretation / limit |
| --- | --- | --- |
| **God of War** (native NVAPI, D3D11) | Sleep tracked frame cadence while On; **0** calls during captured Off intervals while a forced explicit limit still worked. | Off does not imply the configured limiter becomes inactive. |
| **Shadow Warrior 3** (native NVAPI) | **0** observed Sleeps while Off; forced explicit interval still worked. | Confirms the behavior in another native integration. |
| **Hogwarts Legacy** (modern Streamline, D3D12) | **0** while Off, thousands while On/Boost; forced limit continued to work while Off. | Independently reproduces the phenomenon outside native NVAPI. |
| **Satisfactory** (modern Streamline) | **0** while Off; long active intervals roughly 65–73 Sleeps/s. | Compatible with pre-FG/rendered cadence, not conclusive FG telemetry. |
| **DOOM: The Dark Ages** (modern Streamline, Vulkan) | **54,639** setter calls vs **54,638** Sleeps across one run. | Near-perfect 1:1 active loop. |
| **Indiana Jones and the Great Circle** (modern Streamline, Vulkan) | **3,856** setters vs **3,854** Sleeps in one run. | Roughly one each per rendered frame in that interval. |
| **No Man's Sky** (modern Streamline, Vulkan) | **10,674** setters, **0** observed Sleeps in one interval. | Accepted limiter did not govern presentation; mechanism unresolved. |
| **A Plague Tale: Requiem** (legacy SL1) | Zero displayed Sleep counts, but **SL1 Sleep interception is not implemented**. | Not evidence that the game omitted Sleep. |

These are counts at the application-visible intercepted boundary. **Zero calls do not prove that NVIDIA performs no internal sleeping/pacing elsewhere.** In the titles with a working explicit cap and no observed Sleep calls, the exact downstream enforcement mechanism remains unknown.

In one DOOM PT + 2X-FG interval, **2,720 Sleeps over ~35 seconds (~77.7/s)** accompanied approximately **158 FPS displayed output**; Dawnwalker has similar evidence. This is consistent with Sleep being called on *real rendered frames* instead of generated frames, but ReflexProbe does not yet intercept DLSS-G state directly.

## Per-game results

### GSyncProbe (controlled reference)

A controllable D3D11 / D3D12 / Vulkan reference target reproduced ~225 FPS on D3D at 240 Hz with Reflex, VSync, and G-SYNC, versus ~240 FPS on Vulkan without an equivalent automatic cap. It provided direct validation that replacing the explicit Reflex interval with **165, 60, or 30 FPS** changes measured presentation rate, including with Reflex Low Latency Off. Its baseline zero request must not be confused with a game-authored 225 FPS cap.

### Cyberpunk 2077 (GOG)

D3D12, modern Streamline. Repeated setter calls; all tested Reflex modes requested zero.

### The Witcher 3 Remastered

D3D12, modern Streamline. Repeated setter calls; Off, On and On + Boost requested zero.

### A Plague Tale: Requiem (GOG)

D3D12, Streamline 1.0 plugin gateway. Setter calls track roughly frame cadence and request zero across tested modes.

**A Plague Tale: Requiem — Force Boost caveat.** observing and frame-limit overriding through the SL1 plugin path works, but substituting plain On -> On + Boost causes severe progressive performance collapse in Requiem even though the game's native On + Boost mode is healthy. The capability remains exposed for diagnosis; the old SL1 Boost semantics need further investigation.

### Blood of the Dawnwalker (GOG)

UE5.5.3 local build, D3D12, modern dynamically resolved Streamline. Frame Generation enabled caused an Off -> On pair with zero intervals; Frame Generation disabled caused one Off call. With G-SYNC + forced VSync, the FG/Reflex-On case exhibited the familiar ~225 FPS at 240 Hz. **Watch + Inject is runtime-confirmed through GOG Galaxy**: the watcher detected the launcher-created executable, injected successfully, and caught the later sparse Reflex startup calls.

### No Man's Sky (GOG)

Vulkan + modern Streamline with an independent Reflex control. ReflexProbe successfully intercepted **10,674** settings calls in one state interval while counting **0 `slReflexSleep` calls**. A forced nonzero `frameLimitUs` was accepted with a successful result but did not impose the requested presentation limit. The game's own limiter is separate and pre-FG. Zero Streamline sleeps is therefore a concrete difference from Indiana Jones, although it does not by itself prove why the limiter is ineffective.

### Indiana Jones and the Great Circle (GOG)

Vulkan + modern Streamline. Reflex/Frame Generation behavior is gated behind DLSS as the selected upscaler: when DLSS is not selected, the Frame Generation option is hidden and the observed Reflex path is effectively absent. With DLSS active, the game repeatedly submits Reflex On with a zero interval at high frequency. In one 158 FPS run ReflexProbe observed **3,856 settings calls and 3,854 `slReflexSleep` calls**, consistent with approximately one setter and one sleep call per rendered frame during the active gameplay interval. Force Boost and frame-limit override both work, while the game's own limiter remains a separate pre-FG limiter.

### God of War (2018)

D3D11 + native NVAPI Reflex, independent of whether DLSS is enabled. Runtime-confirmed Off / On / On + Boost observation, with `NvAPI_D3D_SetSleepMode` called only when the menu setting changes. While Reflex was On, `NvAPI_D3D_Sleep` counts tracked approximately frame cadence; during the captured Reflex-Off interval the sleep count was **0**, yet the forced `minimumIntervalUs` limiter remained effective. This demonstrates that, in this shipped native-NVAPI integration on the tested driver, application-observed per-frame sleep calls are not required for the explicit limiter to remain active while low-latency mode is Off. Force Boost and `minimumIntervalUs` override both work.

### Shadow Warrior 3 (GOG)

native NVAPI D3D Reflex. The outer `SW3.exe` is an Epic `BootstrapPackagedGame` wrapper, so Launch + Inject targets the wrong process and finds no Reflex path. Watch + Inject against the exact full path `SW3\Binaries\Win64\SW3.exe` catches the real game. SetSleepMode is submitted only on Reflex state changes; frame-limit override and Force Boost work. With Reflex Off, the captured Sleep count was **0** while the forced 6329 us (~158 FPS) limiter still worked. With Reflex On / On + Boost, Sleep counts were approximately frame-cadence. No Frame Generation path is present in the tested title.

### Pragmata (Steam)

D3D12 + modern Streamline 2.8.0. Watch + Inject acquired the Steam-launched executable successfully on the tested Denuvo-protected build, the frame-limit override worked, and Reflex settings were submitted only on option changes while `slReflexSleep` continued at approximately rendered-frame cadence. The tested DRM configuration did not block ReflexProbe's injection/interception path; that observation is specific to this build and is not a claim about every Denuvo integration.

### DOOM: The Dark Ages (Steam; Vulkan / Streamline 2.11.1)

DOOM is now one of the strongest modern Streamline/Vulkan reference integrations.

Steam Watch + Inject succeeds against the real game process. The tested build ships `sl.interposer.dll` and `sl.reflex.dll` 2.11.1. Changing `r_streamlineReflexMinFrameTimeUs` immediately creates a new captured Reflex state with the literal CVar value as the requested `frameLimitUs`.

A large capture produced:

```text
slReflexSetOptions total: 54639
slReflexSleep total:      54638
difference:                   1
```

Completed state intervals were essentially exact 1:1, strongly indicating that id Tech calls SetOptions and Sleep once per iteration of the same active frame loop. The one-call process-lifetime difference is consistent with exit after a final setter but before a corresponding sleep.

The game's exposed performance-statistics `Frame` counter is map-bound rather than process-bound. It resets across fresh map loads, Restart from Checkpoint, and other map-like state transitions, so a final in-game Frame value must not be compared directly with ReflexProbe's process-lifetime counts unless the measurement is deliberately restricted to one map interval.

Path tracing produced an initially confusing but important transition case. Enabling PT did **not** itself create a Reflex state change. Changing the Reflex-min-frame-time CVar under PT immediately produced the expected new SetOptions state and Sleep calls continued, yet the new nonzero limiter could remain visibly ineffective for several seconds before taking effect. In a later fixed-override run, the PT main menu did not visibly obey the forced 6329 us limit, but after loading a gameplay map the ~158 FPS cap engaged and remained solid. The CVar could still change requested state, but ReflexProbe's fixed override kept the effective interval at 6329 us.

One PT/Frame-Generation interval captured:

```text
2720 sleeps over ~35 seconds
≈ 77.7 sleeps/sec
displayed output ≈ 158 FPS
```

That is nearly a 2X relationship, matching the separate Dawnwalker evidence and strongly suggesting that Reflex Sleep tracks **real rendered frames**, not generated presentation frames, under 2X Frame Generation.

The current interpretation is therefore **not** “Path Tracing breaks Reflex.” The better model is that Frame Generation / renderer / presentation state determines whether the explicit Reflex limiter visibly governs final output, while PT can expose a delayed or state-transition “grace window.” Direct FG telemetry is needed to distinguish those states cleanly instead of inferring them from output FPS alone.

### Satisfactory (Steam; modern Streamline)

Satisfactory exposed a real resolver-coverage hole in ReflexProbe rather than an environmental failure. The real engine process must be acquired through **Watch + Inject** because launching `FactoryGameSteam-Win64-Shipping.exe` directly fails; the bootstrapper is required even though the actual Reflex integration lives in the child engine process.

Build `2026-10-06.22-module-resolver-scan` generalized dynamic `GetProcAddress` interception beyond the main EXE to loaded non-system game / engine / plugin modules. That immediately found Satisfactory's previously invisible route:

```text
sl.interposer.dll: 2.7.30.0
resolver caller:    FactoryGameSteam-Core-Win64-Shipping.dll
sl.reflex.dll:      2.14.0.0 from NVIDIA NGX OTA cache
```

The decisive method report was:

```text
modern slGetFeatureFunction via FactoryGameSteam-Core-Win64-Shipping.dll GetProcAddress IAT
```

This proves that the earlier miss was not a missing Reflex integration and was not primarily a Watch timing failure. ReflexProbe had been watching only the main executable's dynamic resolver seam while this UE-derived integration resolved Streamline from another loaded game module.

Satisfactory also produced the first captured commercial example in this test set of a game deliberately using Reflex's own explicit frame limiter. On the main menu, with Frame Generation active, it repeatedly requested:

```text
mode=On
frameLimitUs=16666
≈ 60.002 FPS
```

That menu cap is not exposed as an independent user setting. With ReflexProbe forcing 6329 us, the same calls became effective at ~158.003 FPS, directly overriding the hidden 60 FPS policy. Satisfactory also has a separate conventional pre-Frame-Generation limiter whose normal default can remain unlimited.

The exposed **Frame Generation** option is tightly coupled to Reflex behavior. In the observed test, it was the only user-facing setting found that forced a new Reflex state. Turning Frame Generation Off submitted Reflex Off with a zero interval; turning it back On submitted Reflex On with a zero interval. Menu / initialization transitions also caused rapid `16666 -> 0 -> 16666` policy changes before settling.

Sleep-call counting was coherent but not yet a direct FG measurement. Long active intervals produced approximately:

```text
2708 sleeps / ~37 s ≈ 73.2 sleeps/s
1872 sleeps / ~26 s ≈ 72.0 sleeps/s
1245 sleeps / ~19 s ≈ 65.5 sleeps/s
```

Captured Reflex-Off intervals produced **0 sleeps**. The active rates are consistent with the existing Dawnwalker / DOOM hypothesis that Reflex Sleep follows the real rendered-frame cadence rather than generated presentation frames under 2X FG, but direct DLSS-G state telemetry is still needed before treating the Satisfactory counts as proof.

### Hogwarts Legacy (Steam; D3D12 / modern Streamline)

Hogwarts Legacy is a useful historical bridge specimen: it shipped as a **late Unreal Engine 4** title in February 2023 with the original RTX 40-series style of ordinary 2X Frame Generation, then later received modern Multi Frame Generation support after the RTX 50-series launch. The current tested build supports 2X / 3X / 4X Frame Generation once FG has been enabled; enabling FG from Off requires a restart, while changing between the active multipliers does not. Turning FG fully Off again also requires a restart.

The current build uses D3D12 and resolves modern Streamline through the main executable:

```text
resolver caller:    HogwartsLegacy.exe
sl.interposer.dll:  2.6.10.0
sl.reflex.dll:      2.14.0.0 from NVIDIA NGX OTA cache
```

The observed interception method was:

```text
modern slGetFeatureFunction via HogwartsLegacy.exe GetProcAddress IAT
```

Hogwarts also has its own separate frame-rate limiter. Changing that limiter produced **no Reflex state change**. When configured below ReflexProbe's forced 6329 us (~158.003 FPS) Reflex ceiling, the game's own limiter capped the title further, confirming two independently effective limiting stages.

The more important Reflex behavior is the Off state. During a long captured Reflex-Off interval, Hogwarts produced **0 `slReflexSleep` calls**. After Reflex was enabled, Sleep calls immediately appeared at high frequency; later Off states again produced 0, and re-enabling Reflex restored the calls. Because ReflexProbe had already intercepted and counted thousands of `slReflexSleep` calls before the Off transition, the zeros are not explained by missing the sleep-function resolution.

Despite those zero Sleep calls while Reflex was Off, ReflexProbe's forced nonzero `frameLimitUs` remained effective. This independently reproduces the same key result already seen in God of War's native NVAPI D3D11 path: **application-observed per-frame Reflex Sleep calls are not required for an already-configured explicit Reflex limiter to keep governing frame cadence**. Hogwarts extends that result to modern Streamline on D3D12.

Changing the exposed 2X / 3X / 4X Frame Generation multiplier did not itself produce a Reflex state change. That is expected if Reflex remains continuously enabled while only DLSS-G policy changes, and makes Hogwarts a particularly valuable target for direct `slDLSSGSetOptions` / `slDLSSGGetState` observation.

## Resolver discovery and regression

Some games do not statically import the modern Streamline resolver, and the code that performs dynamic resolution is not necessarily the main executable.

ReflexProbe now has two modern catches:

1. patch a normal import of `slGetFeatureFunction` from `sl.interposer.dll` wherever it appears in a loaded module;
2. patch imported `GetProcAddress` slots across loaded **non-system game / engine / plugin modules**, pass every unrelated lookup through untouched, and substitute `slGetFeatureFunction` only when that symbol is resolved from `sl.interposer.dll`.

The broader resolver scan was added in build `2026-10-06.22-module-resolver-scan`. It deliberately leaves Windows system modules, `ReflexProbe64.dll`, and `sl.interposer.dll`'s dedicated SL1 plugin-gateway path alone. It also refuses to overwrite a `GetProcAddress` IAT slot that no longer points at the canonical Kernel32 resolver, avoiding blind interference with another injector or overlay that already owns that slot.

Blood of the Dawnwalker confirmed the original dynamic path from the main executable. Its local interposer reported `2.7.30.0`, while the loaded Reflex implementation came from NVIDIA's NGX model cache and reported `2.14.0.0`.

Satisfactory then proved why module-wide dynamic discovery is necessary. The main `FactoryGameSteam-Win64-Shipping.exe` process was injected successfully but the old main-EXE-only resolver interception saw nothing even though `sl.reflex.dll` was loaded. The broadened scan immediately found that `FactoryGameSteam-Core-Win64-Shipping.dll` was the module resolving `slGetFeatureFunction`. The interception-method report now includes the actual caller module so this topology is visible in copied logs rather than inferred afterward.

Arming a dynamic-modern `GetProcAddress` interception is **not** treated as completed Reflex discovery. The injected worker continues scanning loaded modules until a concrete Reflex path is captured. This preserves the legacy SL1 case where `sl.interposer.dll` appears later and must have its own `GetProcAddress` import patched before `sl.reflex.dll!slGetPluginFunction("slSetConstants")` is resolved.

The broadened resolver change was regression-tested successfully against three structurally different existing paths:

- **God of War (2018):** native NVAPI D3D path remained healthy, including Force Boost, explicit interval override, and optional Sleep counting.
- **A Plague Tale: Requiem:** Streamline 1.x plugin-gateway interception remained healthy and was not trampled by the modern module scan.
- **DOOM: The Dark Ages:** modern Streamline 2.11.1 Watch interception remained healthy and still resolved through the main EXE, now reported explicitly as `DOOMTheDarkAges.exe GetProcAddress IAT`.

This matters for launcher / Watch mode because a title that submits Reflex options only once or twice at startup must be intercepted before those calls occur, and the relevant resolver can live in a game DLL rather than the process image itself.

### Acquisition hardening: Launch + Inject

An intermittent false failure was reproduced specifically with **Launch + Inject**. The remote `LoadLibraryW` call completed and `ReflexProbe64.dll` went far enough to connect to the controller mapping and publish its injected build stamp, but the controller's single immediate Toolhelp module snapshot still failed to report `ReflexProbe64.dll`. Launch then treated the already-running injection as failed and terminated the game. The failure reproduced even after an earlier successful launch with the same binaries, so it is not considered a fresh-build-only problem.

The hardening is deliberately narrow to reduce regression risk. The existing remote-thread wait, LoadLibrary address calculation, injection mechanics, and immediate module check remain unchanged. Only **Launch + Inject** receives a fallback verification window after an initial module-snapshot miss: for up to one second the controller accepts either the DLL becoming visible in Toolhelp or the injected DLL's existing shared-memory build-stamp handshake as positive proof that injection executed. The remote thread exit code is retained for failure diagnostics but is not used as the sole x64 success test. **Watch + Inject and Attach keep their existing one-shot verification behavior unless testing shows that they need the same fallback.**

Attach remains unverified in the current commercial-game pass. Even a successful late injection cannot retroactively observe a previously issued startup-only setter or replace an already-cached function pointer.

## Instrumentation and maintenance notes

### Capture behavior

ReflexProbe has two **capture modes**, not a capture-versus-display filter.

#### State changes

**State changes** is the default gameplay mode.

Every Reflex call is still intercepted, forwarded and examined, but identical calls are not retained individually. The controller keeps only the current Reflex state plus a 64-bit repeat count.

- the first state is printed as `NEW Reflex State:`;
- identical calls only increment the repeat count;
- when backend, requested mode, effective mode, requested interval, effective interval, or result changes, the previous run is closed with `Previous Reflex State repeated N more times.` and the new state is printed;
- if there were no repeats, no zero-repeat line is emitted;
- target exit or a capture-mode change closes the current state run before continuing.

This makes the normal gameplay footprint essentially independent of whether an engine calls the Reflex setter twice per launch or hundreds of times per second for hours.

#### Raw debug

**Raw debug** is an explicit debugging/tuning mode.

Selecting it starts a new raw capture from that point forward. ReflexProbe does not reconstruct or pretend that calls from before Raw debug was selected were captured. Every subsequently drained Reflex settings call is displayed individually and retained in a fixed **131,072-call RAM ring**. When **Count Reflex Sleep Calls** is enabled, each wrapped sleep call is also displayed and retained individually. Settings and sleep calls keep separate call numbers even though they share the same transport ring. Once the ring is full, the oldest retained raw calls are overwritten by new ones.

The ring is allocated only when Raw debug is selected. With the current event structure it is roughly 6 MiB. Live raw text is still batched at the controller's 50 ms poll cadence, so the Win32 EDIT control receives at most about 20 batch appends per second rather than one mutation per Reflex call.

Switching from State changes to Raw debug first closes the current compressed state run. Switching back stops raw retention and starts a fresh state-change run with the next Reflex call. The most recent raw ring remains in RAM until it is cleared, replaced by a new Raw debug session, or the controller exits.

The **Clear** button empties the visible Status / Reflex requests text and frees any retained Raw debug ring so a new diagnostic pass can start without restarting ReflexProbe. It is intentionally available only while idle: it is disabled while a target is injected and while Watch is armed, so Clear never races the injected DLL's 4096-event shared transport ring or discards a live state/sleep-count run.

**ReflexProbe never writes capture data to disk.** There is intentionally no capture logger or export path. SSDs must live.

The shared transport between the injected DLL and controller remains a fixed 4096-event ring. If the controller is ever lapped, it reports the exact number of lost calls and marks capture continuity as broken rather than silently pretending the stream was complete.

### Supported interception paths

#### Modern Streamline (2.x+)

The injected DLL intercepts `slGetFeatureFunction`, either from a normal loaded-module IAT import or through the narrowly scoped loaded-module `GetProcAddress` path described above. When the game asks for `slReflexSetOptions`, ReflexProbe keeps the genuine NVIDIA function pointer and returns a wrapper that observes requested/effective Reflex mode and frame-limit state. When **Count Reflex Sleep Calls** was selected for the target acquisition, the same resolver can also return a thin `slReflexSleep` wrapper used only for call counting. If counting was not selected, ReflexProbe returns the genuine sleep function pointer untouched. The wrapper can independently replace `frameLimitUs` and upgrade only `eLowLatency` (On) to `eLowLatencyWithBoost`; Off and an existing On + Boost request are left unchanged.

#### Native NVAPI D3D Reflex

Pre-Streamline D3D integrations configure Reflex through `NvAPI_D3D_SetSleepMode`. ReflexProbe patches only the main executable's NVAPI resolution seam: a normal `nvapi_QueryInterface` IAT import, a dynamically obtained `nvapi_QueryInterface` through the already-narrow application `GetProcAddress` hook, or a direct `NvAPI_D3D_SetSleepMode` import if one exists. It recognizes NVIDIA's public SetSleepMode interface ID `0xac1ca9e0` and returns a narrow wrapper for that one function. It does not modify code bytes inside `nvapi64.dll`.

The NVAPI request is normalized into the same Off / On / On + Boost plus requested/effective interval telemetry used by the Streamline backends. When **Count Reflex Sleep Calls** was selected for the target acquisition, native `NvAPI_D3D_Sleep` interface ID `0x852cd1d2` is wrapped for the same optional call counting; otherwise the genuine sleep function is left untouched. Frame-limit override changes `minimumIntervalUs`; Force Boost changes only a native request with low-latency mode enabled and Boost disabled. The caller's complete versioned sleep-mode structure is copied and preserved around those fields.

#### Legacy Streamline (1.x)

Streamline 1.x has another layer between the game's public API and Reflex itself. NVIDIA's `sl.reflex.dll` exports `slGetPluginFunction`, and the Streamline loader asks that gateway for the plugin-private `slSetConstants` function that consumes `ReflexConstants`.

ReflexProbe therefore has two SL1 catches:

1. **Primary SL1 path:** patch only `sl.interposer.dll`'s imported `GetProcAddress`; substitute the plugin gateway when it resolves `sl.reflex.dll!slGetPluginFunction`, then substitute the returned `slSetConstants` function.
2. **Fallback SL1 path:** intercept the older public `slSetFeatureConstants` entry point for Reflex feature ID `3` when a title uses it directly.

The old `ReflexConstants` ABI is defined locally with compile-time layout checks. This path is confirmed working in **A Plague Tale: Requiem**, whose local `sl.reflex.dll` reports version `1.0.0.0`.

### Build identity and regression diagnosis

Every controller run records:

- a human-readable source tag and protocol version
- the controller compile timestamp
- SHA-256 of the running `ReflexProbe.exe`
- SHA-256 of the sibling `ReflexProbe64.dll`
- the compile timestamp reported by the actually loaded injected DLL

The hashes make stale or mismatched local binaries obvious. Interception-method metadata is tracked per resolver/backend path rather than through one shared mutable label, so discovering an unrelated NVAPI path cannot overwrite the method later reported for a Streamline hook (or vice versa).

### Historical sleep-wrapper switches

Earlier diagnostic builds had startup-global `--wrap-reflex-sleep` and `--no-wrap-reflex-sleep` switches implemented with `CommandLineToArgvW`. Those switches and their Shell32 dependency were intentionally removed when sleep interception became an acquisition-time UI choice. The last pre-removal implementation is preserved in Git history at commit `63021c8d3c8f727bdaa7733364eac122589410e5`; the original sleep-counting implementation began at `34f7a00dcf84e0f0f60bb74dbbe46bda53d06de4`. If ReflexProbe needs command-line options again, that working parser can be recovered without keeping unused plumbing in the current binary.

### Remaining experimental questions

Direct `slDLSSGSetOptions` / `slDLSSGGetState` observation is required to distinguish developer-requested FG Off, fullscreen-menu detector suppression, and runtime unavailability; likewise, 2X/3X/4X multiplier changes cannot be inferred from Reflex setter state alone. Important specimens are **Satisfactory** (menu limiter and DLL-origin resolver), **Hogwarts Legacy** (late UE4 fixed-2X to MFG retrofit), **DOOM** (Vulkan/PT transition), **Pragmata** (menu FG behavior), and **Requiem** (older SL1 fixed-2X ABI). Any legacy FG support must use capabilities actually available in that generation.

The test set does not establish the underlying driver pacing mechanism for the Reflex-Off / zero-Sleep / working-limit cases, nor does it identify why No Man's Sky accepted a nonzero interval without following that limit. Those remain open observations rather than resolved explanations.
