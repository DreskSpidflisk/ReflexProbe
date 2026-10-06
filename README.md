# ReflexProbe

ReflexProbe is a deliberately small Win32 tool for observing and overriding the frame-limit value applications send to NVIDIA Reflex.

The current build focuses on **Reflex** in x64 games acquired by direct launch, launcher-compatible process watch, or runtime attach. It supports modern Streamline, legacy Streamline 1.x, and the native NVAPI D3D sleep-mode path used by pre-Streamline integrations. It can independently override Reflex's frame-limit interval and upgrade plain Reflex On requests to On + Boost. It does not render an overlay, modify shaders, replace Streamline DLLs, or write capture data to disk.

## Why this exists

GSyncProbe reproduced a repeatable API split with Reflex enabled and no explicit Reflex frame limit:

- D3D11 @ 240 Hz: about 225 FPS
- D3D12 @ 240 Hz: about 225 FPS
- Vulkan @ 240 Hz: about 240 FPS

UE 5.8.3 source also showed that its native Reflex path normally passes a zero minimum interval unless the engine has an explicit max tick rate. ReflexProbe exists to answer the direct question: **what did the game ask Reflex for, and what happens if we replace that value?**

Modern Streamline exposes the useful value as `sl::ReflexOptions::frameLimitUs`. Streamline 1.x exposed the same value as `sl::ReflexConstants::frameLimitUs`.

- `0` = no explicit Reflex frame-limit interval
- nonzero = explicit minimum frame interval in microseconds

ReflexProbe labels a zero request as `0 us (no explicit frame limit)`. NVIDIA's D3D Reflex/presentation path can still apply its own below-refresh policy downstream, but that behavior is deliberately kept separate from the application's literal API request. Zero is not NULL and is not a request for a particular automatically calculated FPS.

When override mode is enabled, ReflexProbe preserves the game's Reflex mode and every other option, copies the options/constants structure, changes only `frameLimitUs`, and forwards the copy to Streamline. **Zero is a real override value.** If the game requests `6061 us` and ReflexProbe is armed for `0`, NVIDIA receives `0`. If both the game and ReflexProbe choose zero, the numeric value is the same but the override policy still deliberately selected it.

## What we have established so far

### The application is generally passing zero

The below-refresh D3D behavior is **not being produced by the game calculating its own cap** in the samples tested so far.

Evidence includes:

- **UE 5.8.3 native Reflex source:** an uncapped/default client path reaches Reflex with a minimum interval of `0`.
- **GSyncProbe:** the same Streamline request with `frameLimitUs=0`, VSync on and G-SYNC active produces about 225 FPS on D3D11/D3D12 at 240 Hz, while Vulkan reaches about 240 FPS.
- **Cyberpunk 2077 / modern Streamline:** Off, On and On + Boost repeatedly submit `frameLimitUs=0`.
- **The Witcher 3 Remastered / modern Streamline:** the same zero-request pattern appears across Off, On and On + Boost.
- **A Plague Tale: Requiem / Streamline 1.0:** the legacy `sl.reflex!slSetConstants` path likewise submits zero across Off, On and On + Boost.
- **Blood of the Dawnwalker / modern Streamline:** with DLSS Frame Generation enabled the game submitted one Off state followed by one On state, both with `frameLimitUs=0`; with Frame Generation disabled it submitted one Off state. It did not spam the Reflex setter every frame.
- **Indiana Jones and the Great Circle / modern Streamline + Vulkan:** the game repeatedly submits Reflex On with `frameLimitUs=0`. Its Reflex/Frame Generation feature branch is exposed only when DLSS is selected as the upscaler; with a non-DLSS upscaler, Frame Generation disappears from the menu and the observed Reflex behavior is not active. Its own limiter is separate from Reflex and limits the pre-Frame-Generation cadence.
- **No Man's Sky / modern Streamline + Vulkan:** the game exposes an independent Reflex control and submits `frameLimitUs=0`. ReflexProbe can replace that request with a nonzero interval and the setter returns success, but the requested Reflex limit did not govern the observed presentation rate. NMS also has its own separate pre-Frame-Generation limiter.
- **God of War (2018) / native NVAPI D3D11:** the game submits Reflex state only when its menu option changes, supports Off / On / On + Boost, and normally requests a zero minimum interval. ReflexProbe's native `minimumIntervalUs` override worked, including while low-latency mode itself was Off.
- **DOOM: The Dark Ages / modern Streamline 2.11.1 + Vulkan:** `r_streamlineReflexMinFrameTimeUs` maps literally to `frameLimitUs`. Values such as 6050, 6329, 6330, 7000 and 0 were observed directly, and ReflexProbe's 6329 us override replaced every request. DOOM calls `slReflexSetOptions` and `slReflexSleep` essentially 1:1 in the active frame loop.
- **Shadow Warrior 3 (GOG) / native NVAPI D3D Reflex:** the top-level `SW3.exe` is an Epic `BootstrapPackagedGame` wrapper which launches the real `SW3\Binaries\Win64\SW3.exe`. Launch + Inject therefore targets the wrong process, while Watch + Inject with the exact real-game path succeeds. The game submits SetSleepMode only on Reflex state changes; frame-limit override and Force Boost both work.

The commercial samples now show several distinct integration styles: constant setter resubmission, startup/policy-change-only setters, native NVAPI menu-change-only setters, engines that keep their own pre-FG limiter separate from Reflex, launcher/bootstrapper topologies that require exact-path Watch acquisition, and integrations where a nonzero Reflex limiter request is accepted but does not produce the expected pacing.

### Empirical automatic-cap rule

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

### Explicit Reflex frame limits are a separate mechanism

A nonzero Reflex interval is explicit application policy and is independent of whether Reflex Low Latency mode is On. NVIDIA documents the frame limiter as a separate Reflex subfeature, and GSyncProbe confirms the practical behavior: it can submit `mode=Off` while still supplying a nonzero `frameLimitUs` and continuing the Reflex sleep path.

DOOM: The Dark Ages exposes `r_streamlineReflexMinFrameTimeUs`. ReflexProbe runtime capture now proves that this CVar is passed directly to Streamline as `frameLimitUs`: observed requests include `6050`, `6329`, `6330`, `7000`, and `0` microseconds. Setting about `6050 us` targets about 165.3 FPS and produces a rigid presentation cadence on its Vulkan path. A fixed ReflexProbe override at `6329 us` replaced every game request with an effective ~158.003 FPS target. The explicit limiter also remained meaningful with visible Reflex mode Off.

ReflexProbe's own override has now been proven against GSyncProbe. Forcing 165, 60, and 30 FPS replaced GSyncProbe's requested limiter value, and **GSyncProbe's measured App Present Rate followed the forced value**. The same worked while GSyncProbe's Reflex mode was Off because the frame limiter remains independently usable.

### DOOM: The Dark Ages runtime findings

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

### Dynamic modern Streamline resolution is supported

Some games do not statically import the modern Streamline resolver. Blood of the Dawnwalker exposed this topology.

ReflexProbe now has two modern catches:

1. patch a normal application import of `slGetFeatureFunction` from `sl.interposer.dll`;
2. if the main executable resolves Streamline dynamically, patch only the application's imported `GetProcAddress`, pass every unrelated lookup through untouched, and substitute `slGetFeatureFunction` only when it is resolved from `sl.interposer.dll`.

Dawnwalker confirmed the dynamic path. Its local interposer reported `2.7.30.0`, while the loaded Reflex implementation came from NVIDIA's NGX model cache and reported `2.14.0.0`.

This matters for launcher/watch mode because a title that submits Reflex options only once or twice at startup must be intercepted before those calls occur.

Arming the application's dynamic-modern `GetProcAddress` interception is **not** treated as a completed Reflex discovery. The injected worker continues scanning loaded modules until a concrete Reflex path is captured. This preserves the legacy SL1 case where `sl.interposer.dll` appears later and must have its own `GetProcAddress` import patched before `sl.reflex.dll!slGetPluginFunction("slSetConstants")` is resolved. A regression in this worker lifetime briefly broke A Plague Tale: Requiem while modern dynamic titles still worked; build `2026-10-05.11-sl1-worker` restores the patient SL1 scan while retaining Dawnwalker's dynamic-modern path.

## Capture modes

ReflexProbe has two **capture modes**, not a capture-versus-display filter.

### State changes

**State changes** is the default gameplay mode.

Every Reflex call is still intercepted, forwarded and examined, but identical calls are not retained individually. The controller keeps only the current Reflex state plus a 64-bit repeat count.

- the first state is printed as `NEW Reflex State:`;
- identical calls only increment the repeat count;
- when backend, requested mode, effective mode, requested interval, effective interval, or result changes, the previous run is closed with `Previous Reflex State repeated N more times.` and the new state is printed;
- if there were no repeats, no zero-repeat line is emitted;
- target exit or a capture-mode change closes the current state run before continuing.

This makes the normal gameplay footprint essentially independent of whether an engine calls the Reflex setter twice per launch or hundreds of times per second for hours.

### Raw debug

**Raw debug** is an explicit debugging/tuning mode.

Selecting it starts a new raw capture from that point forward. ReflexProbe does not reconstruct or pretend that calls from before Raw debug was selected were captured. Every subsequently drained Reflex settings call is displayed individually and retained in a fixed **131,072-call RAM ring**. When **Count Reflex Sleep Calls** is enabled, each wrapped sleep call is also displayed and retained individually. Settings and sleep calls keep separate call numbers even though they share the same transport ring. Once the ring is full, the oldest retained raw calls are overwritten by new ones.

The ring is allocated only when Raw debug is selected. With the current event structure it is roughly 6 MiB. Live raw text is still batched at the controller's 50 ms poll cadence, so the Win32 EDIT control receives at most about 20 batch appends per second rather than one mutation per Reflex call.

Switching from State changes to Raw debug first closes the current compressed state run. Switching back stops raw retention and starts a fresh state-change run with the next Reflex call. The most recent raw ring remains in RAM until it is cleared, replaced by a new Raw debug session, or the controller exits.

The **Clear** button empties the visible Status / Reflex requests text and frees any retained Raw debug ring so a new diagnostic pass can start without restarting ReflexProbe. It is intentionally available only while idle: it is disabled while a target is injected and while Watch is armed, so Clear never races the injected DLL's 4096-event shared transport ring or discards a live state/sleep-count run.

**ReflexProbe never writes capture data to disk.** There is intentionally no capture logger or export path. SSDs must live.

The shared transport between the injected DLL and controller remains a fixed 4096-event ring. If the controller is ever lapped, it reports the exact number of lost calls and marks capture continuity as broken rather than silently pretending the stream was complete.

## Solution layout

```text
ReflexProbe.sln
    ReflexProbe      - Win32 controller / launcher watcher / injector
    ReflexProbe64    - x64 injected DLL / Streamline Reflex interception

Controller/
    ReflexProbe.cpp      - Win32 UI and acquisition orchestration
    Process.cpp          - process discovery, exact-path matching and injection
    Capture.cpp          - shared transport and capture modes
    Status.cpp           - bounded text/status presentation helpers
    ControllerInternal.h - small controller-internal contract

Common/Protocol.h        - tiny shared-memory protocol
```

Both projects write to:

```text
bin\x64\Debug\
bin\x64\Release\
```

The active Streamline interception path has no third-party runtime or hooking dependency. The repository still contains the pinned MinHook submodule from the first implementation experiment, but ReflexProbe no longer builds or uses it.

## Build prerequisites

- Visual Studio 2022 / v143
- Windows 10 SDK
- x64 only for now
- `STREAMLINE_SDK` environment variable pointing at an NVIDIA Streamline SDK root containing `include\sl_reflex.h`

Open `ReflexProbe.sln` and build Debug or Release x64. Both projects use warning level 4 with warnings treated as errors.

## Build identity in logs

Every controller run records:

- a human-readable source tag and protocol version
- the controller compile timestamp
- SHA-256 of the running `ReflexProbe.exe`
- SHA-256 of the sibling `ReflexProbe64.dll`
- the compile timestamp reported by the actually loaded injected DLL

The hashes make stale or mismatched local binaries obvious. Interception-method metadata is tracked per resolver/backend path rather than through one shared mutable label, so discovering an unrelated NVAPI path cannot overwrite the method later reported for a Streamline hook (or vice versa).

## Streamline generations

### Modern Streamline (2.x+)

The injected DLL intercepts `slGetFeatureFunction`, either from a normal application IAT import or through the narrowly scoped application-`GetProcAddress` path described above. When the game asks for `slReflexSetOptions`, ReflexProbe keeps the genuine NVIDIA function pointer and returns a wrapper that observes requested/effective Reflex mode and frame-limit state. When **Count Reflex Sleep Calls** was selected for the target acquisition, the same resolver can also return a thin `slReflexSleep` wrapper used only for call counting. If counting was not selected, ReflexProbe returns the genuine sleep function pointer untouched. The wrapper can independently replace `frameLimitUs` and upgrade only `eLowLatency` (On) to `eLowLatencyWithBoost`; Off and an existing On + Boost request are left unchanged.

Confirmed modern targets include Cyberpunk 2077, The Witcher 3 Remastered, GSyncProbe, Blood of the Dawnwalker, Indiana Jones and the Great Circle, No Man's Sky, DOOM: The Dark Ages, and Pragmata. The same modern Streamline resolver interception has now worked across D3D12 and Vulkan titles without a Vulkan-specific hook.

### Native NVAPI D3D Reflex

Pre-Streamline D3D integrations configure Reflex through `NvAPI_D3D_SetSleepMode`. ReflexProbe patches only the main executable's NVAPI resolution seam: a normal `nvapi_QueryInterface` IAT import, a dynamically obtained `nvapi_QueryInterface` through the already-narrow application `GetProcAddress` hook, or a direct `NvAPI_D3D_SetSleepMode` import if one exists. It recognizes NVIDIA's public SetSleepMode interface ID `0xac1ca9e0` and returns a narrow wrapper for that one function. It does not modify code bytes inside `nvapi64.dll`.

The NVAPI request is normalized into the same Off / On / On + Boost plus requested/effective interval telemetry used by the Streamline backends. When **Count Reflex Sleep Calls** was selected for the target acquisition, native `NvAPI_D3D_Sleep` interface ID `0x852cd1d2` is wrapped for the same optional call counting; otherwise the genuine sleep function is left untouched. Frame-limit override changes `minimumIntervalUs`; Force Boost changes only a native request with low-latency mode enabled and Boost disabled. The caller's complete versioned sleep-mode structure is copied and preserved around those fields.

**God of War (2018)** runtime-confirms the native-NVAPI/D3D11 backend: menu changes produced Off / On / On + Boost SetSleepMode calls, frame-limit override worked, Force Boost worked, and the explicit limiter remained effective with low-latency mode Off.

**Shadow Warrior 3 (GOG)** independently confirms the native-NVAPI path in a launcher-wrapper topology. Its outer `SW3.exe` is an Epic `BootstrapPackagedGame` executable; the actual game is `SW3\Binaries\Win64\SW3.exe`. Launch + Inject into the outer wrapper finds no Reflex path, while Watch + Inject against the exact full path of the real child process succeeds. Shadow Warrior 3 submits SetSleepMode only when Reflex state changes. With a 6329 us override and Force Boost enabled, plain On became effective On + Boost and the ~158 FPS cap worked. While Reflex was Off, the captured Sleep count was 0 yet the explicit minimum-interval limiter remained effective, reinforcing the separation between Low Latency mode, application-observed Sleep calls, and the configured frame limiter.

### Legacy Streamline (1.x)

Streamline 1.x has another layer between the game's public API and Reflex itself. NVIDIA's `sl.reflex.dll` exports `slGetPluginFunction`, and the Streamline loader asks that gateway for the plugin-private `slSetConstants` function that consumes `ReflexConstants`.

ReflexProbe therefore has two SL1 catches:

1. **Primary SL1 path:** patch only `sl.interposer.dll`'s imported `GetProcAddress`; substitute the plugin gateway when it resolves `sl.reflex.dll!slGetPluginFunction`, then substitute the returned `slSetConstants` function.
2. **Fallback SL1 path:** intercept the older public `slSetFeatureConstants` entry point for Reflex feature ID `3` when a title uses it directly.

The old `ReflexConstants` ABI is defined locally with compile-time layout checks. This path is confirmed working in **A Plague Tale: Requiem**, whose local `sl.reflex.dll` reports version `1.0.0.0`.

## Current workflow

1. Start `ReflexProbe.exe`.
2. Browse to the exact x64 game executable you want to target. **Launch + Inject**, **Watch + Inject**, and **Attach** remain disabled until the selected path names an existing file rather than a directory. Browse validates immediately; a manually edited path is revalidated when Enter is pressed or the edit box loses focus. The acquisition functions still repeat the validation defensively in case the file disappears afterward.
3. Choose a capture mode. **State changes** is the default for normal gameplay; **Raw debug** is for short diagnostic captures. When idle, **Clear** removes the visible history and any retained Raw debug calls without changing the selected capture mode or policy controls.
4. Leave **Override Reflex frame limit** unchecked for pass-through, or check it and enter the desired FPS value. The FPS box is disabled while the override is off and defaults to **158 FPS** when ReflexProbe starts.
5. Optionally enable **Force Boost when Reflex On**. This changes only a plain On request to On + Boost; Off stays Off and an existing On + Boost request stays unchanged.
6. Optionally enable **Count Reflex Sleep Calls**. It defaults Off. This is an **acquisition-time diagnostic choice**: if checked, ReflexProbe wraps the supported Reflex Sleep call for that target and counts it; if unchecked, the sleep function pointer is left untouched. The choice is locked once Launch or Attach successfully acquires a target, and is snapshotted and locked immediately when Watch is armed. It becomes editable again after the target exits, an acquisition fails, or Watch is cancelled. In State changes mode, counted sleep calls are reported when each detected Reflex state ends; in Raw debug, every counted sleep call is printed individually. Calls before the first detected state are intentionally not assigned to a state.
7. Choose one acquisition method:
   - **Launch + Inject** creates the selected executable suspended, injects `ReflexProbe64.dll`, then resumes it.
   - **Watch + Inject** arms a temporary 10 ms process scan, then you launch the game normally through Steam, GOG Galaxy, Ubisoft Connect, Epic, or another launcher. ReflexProbe first filters by executable name, then requires a case-insensitive exact full-path match before opening or injecting the process. The watch stops completely after a match or cancellation.
   - **Attach** performs the same exact-path match once against an already-running process and injects immediately when found.
8. Watch/Attach logs the detected PID, parent PID, full executable path, process-open timing and DLL-injection timing. Command-line capture is intentionally not part of this first implementation.
9. The injected DLL discovers a supported Streamline Reflex boundary and reports requested/effective state to the controller.

Reflex Sleep interception is now strictly opt-in through **Count Reflex Sleep Calls**. The checkbox state is copied into the target's shared state before injection and is immutable for that acquisition. A disabled count therefore means more than "do not record events": ReflexProbe does **not** substitute `slReflexSleep` or `NvAPI_D3D_Sleep` at all. Streamline 1.x sleep interception is not implemented yet.

Earlier diagnostic builds had startup-global `--wrap-reflex-sleep` and `--no-wrap-reflex-sleep` switches implemented with `CommandLineToArgvW`. Those switches and their Shell32 dependency were intentionally removed when sleep interception became an acquisition-time UI choice. The last pre-removal implementation is preserved in Git history at commit `d6439862403a20fc0b094e39945992495c64d99f`; the original sleep-counting implementation began at `9bee5eb7b638e8fb4d1de4cf53c49ef712b5682f`. If ReflexProbe needs command-line options again, that working parser can be recovered without keeping unused plumbing in the current binary.

The controller's single-row policy/acquisition layout has a calculated minimum width based on the fixed policy controls plus the Launch / Watch / Attach group, preventing the controls from overlapping when the window is resized. The Force Boost control is sized to its visible checkbox/label content so the visual gap to Count Reflex Sleep Calls matches the surrounding policy-row spacing rather than inheriting dead button-rectangle width.

The controller also keeps a compact **Current effective state** line and mirrors it in the window title. It updates only after an intercepted Reflex setter returns success and shows the effective mode and effective explicit Reflex FPS limit after ReflexProbe policy has been applied. Repeated identical calls do not churn the UI. A zero interval is shown as **None (0 us)** rather than inventing an FPS, and the display returns to **Unknown** when the target exits.

**Watch is the preferred launcher mode for titles that submit Reflex state only at startup or whose selected executable is only a launcher/bootstrapper.** It has successfully acquired and injected the GOG Galaxy-launched Blood of the Dawnwalker before that title's sparse startup Reflex submissions, and it is required for the tested Shadow Warrior 3 GOG topology: the selected top-level `SW3.exe` is only an Epic `BootstrapPackagedGame` wrapper, while the actual game process appears later at `SW3\Binaries\Win64\SW3.exe`. Exact full-path Watch matching cleanly distinguishes the two identically named processes. Runtime Attach can still be useful for engines that resubmit settings, but attaching after initialization can miss a setter that the game called once and cached before ReflexProbe arrived. Attach has not yet been exercised in the current commercial-game test pass.

### Launch + Inject verification hardening

An intermittent false failure was reproduced specifically with **Launch + Inject**. The remote `LoadLibraryW` call completed and `ReflexProbe64.dll` went far enough to connect to the controller mapping and publish its injected build stamp, but the controller's single immediate Toolhelp module snapshot still failed to report `ReflexProbe64.dll`. Launch then treated the already-running injection as failed and terminated the game. The failure reproduced even after an earlier successful launch with the same binaries, so it is not considered a fresh-build-only problem.

The hardening is deliberately narrow to reduce regression risk. The existing remote-thread wait, LoadLibrary address calculation, injection mechanics, and immediate module check remain unchanged. Only **Launch + Inject** receives a fallback verification window after an initial module-snapshot miss: for up to one second the controller accepts either the DLL becoming visible in Toolhelp or the injected DLL's existing shared-memory build-stamp handshake as positive proof that injection executed. The remote thread exit code is retained for failure diagnostics but is not used as the sole x64 success test. **Watch + Inject and Attach keep their existing one-shot verification behavior unless testing shows that they need the same fallback.**

For a positive FPS override, the controller converts FPS to microseconds using:

```text
frameLimitUs = round(1,000,000 / FPS)
```

Example:

```text
165 FPS -> 6061 us
```

Entering `0` while override is enabled forces literal `frameLimitUs=0`. Frame-limit and Force Boost remain live policies and can be changed while a target is running. Sleep-call counting is different: it is immutable for the acquired target because choosing whether to return a sleep wrapper must happen when the game resolves the function. Watch snapshots frame-limit, Force Boost, and the sleep-count choice when armed so startup-only behavior sees the intended configuration; Launch and Attach snapshot them when acquisition begins.

## Known test targets and results

- **GSyncProbe:** controlled D3D11/D3D12/Vulkan target. ReflexProbe override is proven to change measured presentation rate, including 165, 60 and 30 FPS forced values.
- **Cyberpunk 2077 (GOG):** D3D12, modern Streamline. Repeated setter calls; all tested Reflex modes requested zero.
- **The Witcher 3 Remastered:** D3D12, modern Streamline. Repeated setter calls; Off, On and On + Boost requested zero.
- **A Plague Tale: Requiem (GOG):** D3D12, Streamline 1.0 plugin gateway. Setter calls track roughly frame cadence and request zero across tested modes.
- **Blood of the Dawnwalker (GOG):** UE5.5.3 local build, D3D12, modern dynamically resolved Streamline. Frame Generation enabled caused an Off -> On pair with zero intervals; Frame Generation disabled caused one Off call. With G-SYNC + forced VSync, the FG/Reflex-On case exhibited the familiar ~225 FPS at 240 Hz. **Watch + Inject is runtime-confirmed through GOG Galaxy**: the watcher detected the launcher-created executable, injected successfully, and caught the later sparse Reflex startup calls.
- **No Man's Sky (GOG):** Vulkan + modern Streamline with an independent Reflex control. ReflexProbe successfully intercepted **10,674** settings calls in one state interval while counting **0 `slReflexSleep` calls**. A forced nonzero `frameLimitUs` was accepted with a successful result but did not impose the requested presentation limit. The game's own limiter is separate and pre-FG. Zero Streamline sleeps is therefore a concrete difference from Indiana Jones, although it does not by itself prove why the limiter is ineffective.
- **Indiana Jones and the Great Circle (GOG):** Vulkan + modern Streamline. Reflex/Frame Generation behavior is gated behind DLSS as the selected upscaler: when DLSS is not selected, the Frame Generation option is hidden and the observed Reflex path is effectively absent. With DLSS active, the game repeatedly submits Reflex On with a zero interval at high frequency. In one 158 FPS run ReflexProbe observed **3,856 settings calls and 3,854 `slReflexSleep` calls**, consistent with approximately one setter and one sleep call per rendered frame during the active gameplay interval. Force Boost and frame-limit override both work, while the game's own limiter remains a separate pre-FG limiter.
- **God of War (2018):** D3D11 + native NVAPI Reflex, independent of whether DLSS is enabled. Runtime-confirmed Off / On / On + Boost observation, with `NvAPI_D3D_SetSleepMode` called only when the menu setting changes. While Reflex was On, `NvAPI_D3D_Sleep` counts tracked approximately frame cadence; during the captured Reflex-Off interval the sleep count was **0**, yet the forced `minimumIntervalUs` limiter remained effective. This demonstrates that, in this shipped native-NVAPI integration on the tested driver, application-observed per-frame sleep calls are not required for the explicit limiter to remain active while low-latency mode is Off. Force Boost and `minimumIntervalUs` override both work.
- **DOOM: The Dark Ages (Steam):** Vulkan + modern Streamline 2.11.1. Watch + Inject succeeds. `r_streamlineReflexMinFrameTimeUs` maps literally to Reflex `frameLimitUs`, and a fixed 6329 us override replaces arbitrary game requests with an effective ~158.003 FPS target. One process-lifetime capture observed **54,639 SetOptions calls and 54,638 Sleep calls**, essentially exact 1:1. Under Path Tracing, state changes and Sleep calls continue immediately even when the presentation cap takes several seconds or a map transition to become visibly effective. In one PT + 2X-FG interval, **2,720 sleeps over ~35 seconds (~77.7/s)** accompanied ~158 FPS displayed output, strongly indicating that Reflex Sleep tracks real rendered frames rather than generated output frames.
- **Shadow Warrior 3 (GOG):** native NVAPI D3D Reflex. The outer `SW3.exe` is an Epic `BootstrapPackagedGame` wrapper, so Launch + Inject targets the wrong process and finds no Reflex path. Watch + Inject against the exact full path `SW3\Binaries\Win64\SW3.exe` catches the real game. SetSleepMode is submitted only on Reflex state changes; frame-limit override and Force Boost work. With Reflex Off, the captured Sleep count was **0** while the forced 6329 us (~158 FPS) limiter still worked. With Reflex On / On + Boost, Sleep counts were approximately frame-cadence. No Frame Generation path is present in the tested title.
- **A Plague Tale: Requiem Force Boost caveat:** observing and frame-limit overriding through the SL1 plugin path works, but substituting plain On -> On + Boost causes severe progressive performance collapse in Requiem even though the game's native On + Boost mode is healthy. The capability remains exposed for diagnosis; the old SL1 Boost semantics need further investigation.
- **Pragmata (Steam):** D3D12 + modern Streamline 2.8.0. Watch + Inject acquired the Steam-launched executable successfully on the tested Denuvo-protected build, the frame-limit override worked, and Reflex settings were submitted only on option changes while `slReflexSleep` continued at approximately rendered-frame cadence. The tested DRM configuration did not block ReflexProbe's injection/interception path; that observation is specific to this build and is not a claim about every Denuvo integration.

## Deliberate limitations

- x64 only
- Streamline plus native NVAPI D3D SetSleepMode interception
- no disk capture or export path
- Raw debug retains only the latest 131,072 raw calls
- the visible log is a standard Win32 EDIT control with a 16 MiB text limit
- Watch currently uses low-overhead Toolhelp process polling rather than a kernel/ETW process-start notification path
- Attach cannot recover Reflex setter calls that occurred before injection and has not yet been runtime-tested in the current commercial-game pass
- **Launch + Inject** previously had an intermittent post-`LoadLibraryW` verification false negative: the injected DLL could report its build stamp through shared memory while the controller's immediate Toolhelp module snapshot missed it. The Launch path now has a bounded fallback verifier for that specific case. Watch has not exhibited the issue in testing and Attach remains untested, so their verification path is intentionally unchanged
- Streamline 1.x sleep-call interception is not implemented
- no native Vulkan `VK_NV_low_latency2` hook yet
- no anti-cheat support

Do not use ReflexProbe with anti-cheat/protected multiplayer titles. The intended test set is offline/no-anti-cheat software where process injection is not fighting a protection system.

## Planned next steps

1. Regression-test the hardened **Launch + Inject** verification across fresh and repeated launches while confirming that Watch behavior remains unchanged.
2. Continue **Watch + Inject** coverage beyond the successful GOG Galaxy / Dawnwalker case, especially Steam and other launcher-owned games.
3. Runtime-test **Attach** and document which titles resubmit enough Reflex state for a late attach to be useful.
4. Expand supported sleep-call comparisons to Dawnwalker and other integrations now that No Man's Sky, Indiana Jones, and God of War have produced sharply different patterns; add an SL1 sleep interception path before treating Requiem's displayed zero as a measurement.
5. Use DOOM: The Dark Ages as a primary modern Vulkan/Frame-Generation validation target. Directly correlate FG state with the observed PT/menu/gameplay “grace window” so the presentation transition can be explained from telemetry rather than inferred from FPS.
6. Add native `VK_NV_low_latency2` interception only if a Vulkan title bypasses the already-working modern Streamline resolver path.
7. Extend the same Streamline resolver architecture to observe DLSS Frame Generation policy such as `slDLSSGSetOptions` / state queries, with capability-aware handling for both modern Streamline and legacy pre-MFG integrations.

The architecture remains intentionally boring: a Win32 controller outside the game, one injected DLL inside it, Launch/Watch/Attach as interchangeable acquisition paths, fixed RAM transport, bounded capture policy, and a tiny requested/effective Reflex policy surface under the microscope.
