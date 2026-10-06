# ReflexProbe

ReflexProbe is a deliberately small Win32 tool for observing and overriding the frame-limit value applications send to NVIDIA Reflex.

The current build focuses on **Streamline Reflex** and x64 games acquired by direct launch, launcher-compatible process watch, or runtime attach. It does not render an overlay, modify shaders, replace Streamline DLLs, or write capture data to disk.

## Why this exists

GSyncProbe reproduced a repeatable API split with Reflex enabled and no explicit Reflex frame limit:

- D3D11 @ 240 Hz: about 225 FPS
- D3D12 @ 240 Hz: about 225 FPS
- Vulkan @ 240 Hz: about 240 FPS

UE 5.8.3 source also showed that its native Reflex path normally passes a zero minimum interval unless the engine has an explicit max tick rate. ReflexProbe exists to answer the direct question: **what did the game ask Reflex for, and what happens if we replace that value?**

Modern Streamline exposes the useful value as `sl::ReflexOptions::frameLimitUs`. Streamline 1.x exposed the same value as `sl::ReflexConstants::frameLimitUs`.

- `0` = no explicit Reflex frame-limit interval
- nonzero = explicit minimum frame interval in microseconds

ReflexProbe still labels a zero request as `automatic` in the log because NVIDIA's D3D Reflex/presentation path can apply its own below-refresh policy downstream. The API value itself is still literal zero; it is not NULL and it is not a request for a particular automatically calculated FPS.

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

This gives us two useful commercial integration styles: engines that resubmit settings constantly, and engines such as Dawnwalker that appear to set state only when policy changes.

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

DOOM: The Dark Ages exposes `j_streamlineReflexMinFrameTimeUs`. Setting about `6050 us` targets about 165.3 FPS and produces a rigid presentation cadence even on its Vulkan path.

ReflexProbe's own override has now been proven against GSyncProbe. Forcing 165, 60, and 30 FPS replaced GSyncProbe's requested limiter value, and **GSyncProbe's measured App Present Rate followed the forced value**. The same worked while GSyncProbe's Reflex mode was Off because the frame limiter remains independently usable.

### Dynamic modern Streamline resolution is supported

Some games do not statically import the modern Streamline resolver. Blood of the Dawnwalker exposed this topology.

ReflexProbe now has two modern catches:

1. patch a normal application import of `slGetFeatureFunction` from `sl.interposer.dll`;
2. if the main executable resolves Streamline dynamically, patch only the application's imported `GetProcAddress`, pass every unrelated lookup through untouched, and substitute `slGetFeatureFunction` only when it is resolved from `sl.interposer.dll`.

Dawnwalker confirmed the dynamic path. Its local interposer reported `2.7.30.0`, while the loaded Reflex implementation came from NVIDIA's NGX model cache and reported `2.14.0.0`.

This matters for the planned launcher/watch mode because a title that submits Reflex options only once or twice at startup must be intercepted before those calls occur.

## Capture modes

ReflexProbe has two **capture modes**, not a capture-versus-display filter.

### State changes

**State changes** is the default gameplay mode.

Every Reflex call is still intercepted, forwarded and examined, but identical calls are not retained individually. The controller keeps only the current Reflex state plus a 64-bit repeat count.

- the first state is printed as `NEW Reflex State:`;
- identical calls only increment the repeat count;
- when backend, mode, requested interval, effective interval, or result changes, the previous run is closed with `Previous Reflex State repeated N more times.` and the new state is printed;
- if there were no repeats, no zero-repeat line is emitted;
- target exit or a capture-mode change closes the current state run before continuing.

This makes the normal gameplay footprint essentially independent of whether an engine calls the Reflex setter twice per launch or hundreds of times per second for hours.

### Raw debug

**Raw debug** is an explicit debugging/tuning mode.

Selecting it starts a new raw capture from that point forward. ReflexProbe does not reconstruct or pretend that calls from before Raw debug was selected were captured. Every subsequently drained Reflex call is displayed individually and retained in a fixed **131,072-call RAM ring**. Once the ring is full, the oldest retained raw calls are overwritten by new ones.

The ring is allocated only when Raw debug is selected. With the current compact event structure it is roughly 4 MiB. Live raw text is still batched at the controller's 50 ms poll cadence, so the Win32 EDIT control receives at most about 20 batch appends per second rather than one mutation per Reflex call.

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

The injected DLL intercepts `slGetFeatureFunction`, either from a normal application IAT import or through the narrowly scoped application-`GetProcAddress` path described above. When the game asks for `slReflexSetOptions`, ReflexProbe keeps the genuine NVIDIA function pointer and returns a wrapper that observes and optionally replaces only `frameLimitUs`.

Confirmed modern targets include Cyberpunk 2077, The Witcher 3 Remastered, GSyncProbe, and Blood of the Dawnwalker.

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
4. Leave **Override Reflex frame limit** unchecked for observation, or check it and enter the desired FPS value.
5. Choose one acquisition method:
   - **Launch + Inject** creates the selected executable suspended, injects `ReflexProbe64.dll`, then resumes it.
   - **Watch + Inject** arms a temporary 10 ms process scan, then you launch the game normally through Steam, GOG Galaxy, Ubisoft Connect, Epic, or another launcher. ReflexProbe first filters by executable name, then requires a case-insensitive exact full-path match before opening or injecting the process. The watch stops completely after a match or cancellation.
   - **Attach** performs the same exact-path match once against an already-running process and injects immediately when found.
6. Watch/Attach logs the detected PID, parent PID, full executable path, process-open timing and DLL-injection timing. Command-line capture is intentionally not part of this first implementation.
7. The injected DLL discovers a supported Streamline Reflex boundary and reports requested/effective state to the controller.

**Watch is the preferred launcher mode for titles that submit Reflex state only at startup.** Runtime Attach can still be useful for engines that resubmit settings, but attaching after initialization can miss a setter that the game called once and cached before ReflexProbe arrived.

For a positive FPS override, the controller converts FPS to microseconds using:

```text
frameLimitUs = round(1,000,000 / FPS)
```

Example:

```text
165 FPS -> 6061 us
```

Entering `0` while override is enabled forces literal `frameLimitUs=0`. Changing the override while the game is running updates shared configuration immediately, but the new value takes effect on the next intercepted Reflex settings call.

## Known test targets and results

- **GSyncProbe:** controlled D3D11/D3D12/Vulkan target. ReflexProbe override is proven to change measured presentation rate, including 165, 60 and 30 FPS forced values.
- **Cyberpunk 2077 (GOG):** D3D12, modern Streamline. Repeated setter calls; all tested Reflex modes requested zero.
- **The Witcher 3 Remastered:** D3D12, modern Streamline. Repeated setter calls; Off, On and On + Boost requested zero.
- **A Plague Tale: Requiem (GOG):** D3D12, Streamline 1.0 plugin gateway. Setter calls track roughly frame cadence and request zero across tested modes.
- **Blood of the Dawnwalker (GOG):** UE5.5.3 local build, D3D12, modern dynamically resolved Streamline. Frame Generation enabled caused an Off -> On pair with zero intervals; Frame Generation disabled caused one Off call. With G-SYNC + forced VSync, the FG/Reflex-On case exhibited the familiar ~225 FPS at 240 Hz.
- **No Man's Sky:** Vulkan + Streamline, DLSS Frame Generation and an independent Reflex control. Planned Vulkan comparison target.
- **Indiana Jones and the Great Circle:** Vulkan + Frame Generation, no exposed Reflex control. Useful future target for discovering whether its menu limiter drives Reflex `frameLimitUs` or a separate engine limiter.
- **God of War (2018):** unusual D3D11 + Reflex target; native NVAPI backend remains future work.
- **Pragmata and other launcher titles:** practical Watch + Inject targets. Blood of the Dawnwalker under GOG Galaxy is an immediate launcher-owned test case because its Reflex state is only submitted at startup/policy changes.

## Deliberate limitations

- x64 only
- Streamline interception only
- no disk capture or export path
- Raw debug retains only the latest 131,072 raw calls
- the visible log is a standard Win32 EDIT control with a 16 MiB text limit
- Watch currently uses low-overhead Toolhelp process polling rather than a kernel/ETW process-start notification path
- Attach cannot recover Reflex setter calls that occurred before injection
- no native NVAPI hook yet
- no native Vulkan hook yet
- no anti-cheat support

Do not use ReflexProbe with anti-cheat/protected multiplayer titles. The intended test set is offline/no-anti-cheat software where process injection is not fighting a protection system.

## Planned next steps

1. Exercise **Watch + Inject** against GOG Galaxy / Blood of the Dawnwalker, then Steam and other launcher-owned games.
2. Build a tiny synthetic one-shot Reflex target/launcher to measure how quickly the 10 ms watcher acquires and injects before a startup-only `slReflexSetOptions` call.
3. If ordinary low-overhead process polling actually loses that race, evaluate a lower-latency Windows process-start notification path rather than guessing in advance.
4. Expand Vulkan coverage and later add native `VK_NV_low_latency2` interception when required.
5. Add native NVAPI D3D Reflex interception for titles such as God of War.
6. After launcher acquisition is proven, extend the same modern Streamline resolver interception to observe DLSS Frame Generation policy such as `slDLSSGSetOptions`.

The architecture remains intentionally boring: a Win32 controller outside the game, one injected DLL inside it, Launch/Watch/Attach as interchangeable acquisition paths, fixed RAM transport, bounded capture policy, and one Reflex frame-limit field under the microscope.
