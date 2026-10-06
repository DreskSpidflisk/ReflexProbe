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

The commercial samples now show several distinct integration styles: constant setter resubmission, startup/policy-change-only setters, native NVAPI menu-change-only setters, engines that keep their own pre-FG limiter separate from Reflex, and integrations where a nonzero Reflex limiter request is accepted but does not produce the expected pacing.

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

DOOM: The Dark Ages exposes `r_streamlineReflexMinFrameTimeUs`. Setting about `6050 us` targets about 165.3 FPS and produces a rigid presentation cadence even on its Vulkan path.

ReflexProbe's own override has now been proven against GSyncProbe. Forcing 165, 60, and 30 FPS replaced GSyncProbe's requested limiter value, and **GSyncProbe's measured App Present Rate followed the forced value**. The same worked while GSyncProbe's Reflex mode was Off because the frame limiter remains independently usable.

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

Switching from State changes to Raw debug first closes the current compressed state run. Switching back stops raw retention and starts a fresh state-change run with the next Reflex call. The most recent raw ring remains in RAM until it is replaced by a new Raw debug session or the controller exits.

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

The hashes make stale or mismatched local binaries obvious.

## Streamline generations

### Modern Streamline (2.x+)

The injected DLL intercepts `slGetFeatureFunction`, either from a normal application IAT import or through the narrowly scoped application-`GetProcAddress` path described above. When the game asks for `slReflexSetOptions`, ReflexProbe keeps the genuine NVIDIA function pointer and returns a wrapper that observes requested/effective Reflex mode and frame-limit state. When sleep wrapping is enabled, the same resolver can also return a thin `slReflexSleep` wrapper used only for optional call counting. The wrapper can independently replace `frameLimitUs` and upgrade only `eLowLatency` (On) to `eLowLatencyWithBoost`; Off and an existing On + Boost request are left unchanged.

Confirmed modern targets include Cyberpunk 2077, The Witcher 3 Remastered, GSyncProbe, Blood of the Dawnwalker, Indiana Jones and the Great Circle, and No Man's Sky. The same modern Streamline resolver interception has now worked across D3D12 and Vulkan titles without a Vulkan-specific hook.

### Native NVAPI D3D Reflex

Pre-Streamline D3D integrations configure Reflex through `NvAPI_D3D_SetSleepMode`. ReflexProbe patches only the main executable's NVAPI resolution seam: a normal `nvapi_QueryInterface` IAT import, a dynamically obtained `nvapi_QueryInterface` through the already-narrow application `GetProcAddress` hook, or a direct `NvAPI_D3D_SetSleepMode` import if one exists. It recognizes NVIDIA's public SetSleepMode interface ID `0xac1ca9e0` and returns a narrow wrapper for that one function. It does not modify code bytes inside `nvapi64.dll`.

The NVAPI request is normalized into the same Off / On / On + Boost plus requested/effective interval telemetry used by the Streamline backends. When sleep wrapping is enabled, native `NvAPI_D3D_Sleep` interface ID `0x852cd1d2` is wrapped for the same optional call counting. Frame-limit override changes `minimumIntervalUs`; Force Boost changes only a native request with low-latency mode enabled and Boost disabled. The caller's complete versioned sleep-mode structure is copied and preserved around those fields.

**God of War (2018)** runtime-confirms the native-NVAPI/D3D11 backend: menu changes produced Off / On / On + Boost SetSleepMode calls, frame-limit override worked, Force Boost worked, and the explicit limiter remained effective with low-latency mode Off.

### Legacy Streamline (1.x)

Streamline 1.x has another layer between the game's public API and Reflex itself. NVIDIA's `sl.reflex.dll` exports `slGetPluginFunction`, and the Streamline loader asks that gateway for the plugin-private `slSetConstants` function that consumes `ReflexConstants`.

ReflexProbe therefore has two SL1 catches:

1. **Primary SL1 path:** patch only `sl.interposer.dll`'s imported `GetProcAddress`; substitute the plugin gateway when it resolves `sl.reflex.dll!slGetPluginFunction`, then substitute the returned `slSetConstants` function.
2. **Fallback SL1 path:** intercept the older public `slSetFeatureConstants` entry point for Reflex feature ID `3` when a title uses it directly.

The old `ReflexConstants` ABI is defined locally with compile-time layout checks. This path is confirmed working in **A Plague Tale: Requiem**, whose local `sl.reflex.dll` reports version `1.0.0.0`.

## Current workflow

1. Start `ReflexProbe.exe`.
2. Browse to the exact x64 game executable you want to target.
3. Choose a capture mode. **State changes** is the default for normal gameplay; **Raw debug** is for short diagnostic captures.
4. Leave **Override Reflex frame limit** unchecked for pass-through, or check it and enter the desired FPS value. The FPS box is disabled while the override is off and defaults to **158 FPS** when ReflexProbe starts.
5. Optionally enable **Force Boost when Reflex On**. This changes only a plain On request to On + Boost; Off stays Off and an existing On + Boost request stays unchanged.
6. Optionally enable **Count Reflex Sleep Calls**. It defaults Off. In State changes mode, wrapped sleep calls are counted during each detected Reflex state and reported when that state ends; in Raw debug, every counted sleep call is printed individually. Calls before the first detected state are intentionally not assigned to a state.
7. Choose one acquisition method:
   - **Launch + Inject** creates the selected executable suspended, injects `ReflexProbe64.dll`, then resumes it.
   - **Watch + Inject** arms a temporary 10 ms process scan, then you launch the game normally through Steam, GOG Galaxy, Ubisoft Connect, Epic, or another launcher. ReflexProbe first filters by executable name, then requires a case-insensitive exact full-path match before opening or injecting the process. The watch stops completely after a match or cancellation.
   - **Attach** performs the same exact-path match once against an already-running process and injects immediately when found.
8. Watch/Attach logs the detected PID, parent PID, full executable path, process-open timing and DLL-injection timing. Command-line capture is intentionally not part of this first implementation.
9. The injected DLL discovers a supported Streamline Reflex boundary and reports requested/effective state to the controller.

Reflex Sleep wrapping is enabled by default for the current diagnostic phase, while counting remains opt-in. Start ReflexProbe with `--no-wrap-reflex-sleep` to return genuine sleep function pointers untouched; the **Count Reflex Sleep Calls** checkbox remains visible but disabled. `--wrap-reflex-sleep` explicitly selects wrapping and exists so the default can be inverted later without changing the command-line contract. Supplying both switches is an error. Streamline 1.x sleep interception is not implemented yet.

The controller also keeps a compact **Current effective state** line and mirrors it in the window title. It updates only after an intercepted Reflex setter returns success and shows the effective mode and effective explicit Reflex FPS limit after ReflexProbe policy has been applied. Repeated identical calls do not churn the UI. A zero interval is shown as **None (0 us)** rather than inventing an FPS, and the display returns to **Unknown** when the target exits.

**Watch is the preferred launcher mode for titles that submit Reflex state only at startup.** It has now successfully acquired and injected the GOG Galaxy-launched Blood of the Dawnwalker before that title's sparse startup Reflex submissions. Runtime Attach can still be useful for engines that resubmit settings, but attaching after initialization can miss a setter that the game called once and cached before ReflexProbe arrived. Attach has not yet been exercised in the current commercial-game test pass.

For a positive FPS override, the controller converts FPS to microseconds using:

```text
frameLimitUs = round(1,000,000 / FPS)
```

Example:

```text
165 FPS -> 6061 us
```

Entering `0` while override is enabled forces literal `frameLimitUs=0`. Frame-limit, Force Boost, and sleep-call counting are independent and can be changed while a wrapped target is running. Watch snapshots all three live policies when armed so startup-only behavior sees the intended configuration. The wrap/no-wrap decision itself is immutable for a controller run and is set only by the controller command line.

## Known test targets and results

- **GSyncProbe:** controlled D3D11/D3D12/Vulkan target. ReflexProbe override is proven to change measured presentation rate, including 165, 60 and 30 FPS forced values.
- **Cyberpunk 2077 (GOG):** D3D12, modern Streamline. Repeated setter calls; all tested Reflex modes requested zero.
- **The Witcher 3 Remastered:** D3D12, modern Streamline. Repeated setter calls; Off, On and On + Boost requested zero.
- **A Plague Tale: Requiem (GOG):** D3D12, Streamline 1.0 plugin gateway. Setter calls track roughly frame cadence and request zero across tested modes.
- **Blood of the Dawnwalker (GOG):** UE5.5.3 local build, D3D12, modern dynamically resolved Streamline. Frame Generation enabled caused an Off -> On pair with zero intervals; Frame Generation disabled caused one Off call. With G-SYNC + forced VSync, the FG/Reflex-On case exhibited the familiar ~225 FPS at 240 Hz. **Watch + Inject is runtime-confirmed through GOG Galaxy**: the watcher detected the launcher-created executable, injected successfully, and caught the later sparse Reflex startup calls.
- **No Man's Sky (GOG):** Vulkan + modern Streamline with an independent Reflex control. ReflexProbe successfully intercepts its settings calls and can forward an overridden nonzero `frameLimitUs` with a successful result, but the forced Reflex interval did not impose the requested presentation limit in testing. The game's own limiter is separate and pre-FG. Optional `slReflexSleep` call counting was added specifically to investigate whether its sleep path explains this behavior.
- **Indiana Jones and the Great Circle (GOG):** Vulkan + modern Streamline. Reflex/Frame Generation behavior is gated behind DLSS as the selected upscaler: when DLSS is not selected, the Frame Generation option is hidden and the observed Reflex path is effectively absent. With DLSS active, the game repeatedly submits Reflex On with a zero interval at high frequency. Force Boost and frame-limit override are both intercepted correctly, while the game's own limiter remains a separate pre-FG limiter.
- **God of War (2018):** D3D11 + native NVAPI Reflex, independent of whether DLSS is enabled. Runtime-confirmed Off / On / On + Boost observation, with `NvAPI_D3D_SetSleepMode` called only when the menu setting changes. Force Boost and `minimumIntervalUs` override both work, and the explicit Reflex limiter remains effective with low-latency mode Off.
- **A Plague Tale: Requiem Force Boost caveat:** observing and frame-limit overriding through the SL1 plugin path works, but substituting plain On -> On + Boost causes severe progressive performance collapse in Requiem even though the game's native On + Boost mode is healthy. The capability remains exposed for diagnosis; the old SL1 Boost semantics need further investigation.
- **Pragmata and other launcher titles:** practical future Watch + Inject targets after the GOG Galaxy Dawnwalker success.

## Deliberate limitations

- x64 only
- Streamline plus native NVAPI D3D SetSleepMode interception
- no disk capture or export path
- Raw debug retains only the latest 131,072 raw calls
- the visible log is a standard Win32 EDIT control with a 16 MiB text limit
- Watch currently uses low-overhead Toolhelp process polling rather than a kernel/ETW process-start notification path
- Attach cannot recover Reflex setter calls that occurred before injection and has not yet been runtime-tested in the current commercial-game pass
- an intermittent **Launch + Inject** post-build verification race has been observed: the injected DLL can report its build stamp through shared memory, proving it executed, while the controller's immediate one-shot Toolhelp module snapshot still fails to see `ReflexProbe64.dll` and falsely reports injection failure. This has only been observed with Launch so far; Watch has not exhibited it in testing, and Attach remains untested
- Streamline 1.x sleep-call interception is not implemented
- no native Vulkan `VK_NV_low_latency2` hook yet
- no anti-cheat support

Do not use ReflexProbe with anti-cheat/protected multiplayer titles. The intended test set is offline/no-anti-cheat software where process injection is not fighting a protection system.

## Planned next steps

1. Harden **Launch + Inject** post-`LoadLibraryW` verification without changing the working injection/interception path: retain the existing module check, add a short bounded retry, and treat the injected shared-memory build stamp as an independent positive proof that ReflexProbe64 executed.
2. Continue **Watch + Inject** coverage beyond the successful GOG Galaxy / Dawnwalker case, especially Steam and other launcher-owned games.
3. Runtime-test **Attach** and document which titles resubmit enough Reflex state for a late attach to be useful.
4. Use the new optional sleep-call counting on No Man's Sky, Indiana Jones, God of War, and other targets to compare setter cadence with the actual Reflex sleep path.
5. Try DOOM: The Dark Ages through Steam and compare its explicit `r_streamlineReflexMinFrameTimeUs` behavior with the zero-request commercial integrations.
6. Add native `VK_NV_low_latency2` interception only if a Vulkan title bypasses the already-working modern Streamline resolver path.
7. After launcher acquisition is sufficiently exercised, extend the same modern Streamline resolver interception to observe DLSS Frame Generation policy such as `slDLSSGSetOptions`.

The architecture remains intentionally boring: a Win32 controller outside the game, one injected DLL inside it, Launch/Watch/Attach as interchangeable acquisition paths, fixed RAM transport, bounded capture policy, and a tiny requested/effective Reflex policy surface under the microscope.
