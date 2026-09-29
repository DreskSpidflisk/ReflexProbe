# ReflexProbe

ReflexProbe is a deliberately small Win32 tool for observing and overriding the frame-limit value applications send to NVIDIA Reflex.

The first bootstrap build focuses on **Streamline Reflex** and **directly launched x64 games**. It does not render an overlay, modify shaders, replace Streamline DLLs, or know anything about Steam/GOG/Ubisoft launchers.

## Why this exists

GSyncProbe reproduced a repeatable API split with Reflex enabled and no explicit Reflex frame limit:

- D3D11 @ 240 Hz: about 225 FPS
- D3D12 @ 240 Hz: about 225 FPS
- Vulkan @ 240 Hz: about 240 FPS

UE 5.8.3 source also showed that its native Reflex path normally passes a zero minimum interval unless the engine has an explicit max tick rate. ReflexProbe exists to answer the next question directly: **what did the game actually ask Reflex for?**

Modern Streamline exposes the useful value as `sl::ReflexOptions::frameLimitUs`. Streamline 1.x exposed the same value as `sl::ReflexConstants::frameLimitUs`.

- `0` = no explicit Reflex frame limit / NVIDIA automatic behavior
- nonzero = explicit minimum frame interval in microseconds

When override mode is enabled, ReflexProbe preserves the game's Reflex mode and every other option, copies the options/constants structure, changes only `frameLimitUs`, and forwards the copy to Streamline.

## What we have established so far

### The application is generally passing zero

The most important finding is that the below-refresh D3D behavior is **not being produced by the game calculating its own cap** in the samples tested so far.

Evidence now includes:

- **UE 5.8.3 native Reflex source:** the engine converts an explicit `DesiredMaxTickRate` to `minimumIntervalUs`, but an uncapped/default client path reaches Reflex with a minimum interval of `0`.
- **GSyncProbe:** the same Streamline Reflex request with `frameLimitUs=0`, VSync on and G-SYNC active produces about 225 FPS on D3D11/D3D12 at 240 Hz, while Vulkan reaches about 240 FPS.
- **Cyberpunk 2077 / modern Streamline:** ReflexProbe observed `slReflexSetOptions` continuously with `frameLimitUs=0` for Off, On and On + Boost.
- **The Witcher 3 Remastered / modern Streamline:** the new REDengine backport shows the same pattern: Off, On and On + Boost all continue to submit `frameLimitUs=0`.
- **A Plague Tale: Requiem / Streamline 1.0:** ReflexProbe now catches the legacy `sl.reflex!slSetConstants` path. Off, On and On + Boost all submit `frameLimitUs=0`.

This is the central operational result behind ReflexProbe: **the application can submit zero while NVIDIA's D3D Reflex/presentation path still caps below refresh.** Vulkan, under the otherwise-equivalent GSyncProbe test, does not perform that same automatic below-refresh cap.

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

A nonzero Reflex interval is explicit application policy and is independent of the automatic D3D behavior above.

For example, DOOM: The Dark Ages exposes `j_streamlineReflexMinFrameTimeUs`. Setting approximately `6050 us` targets about 165.3 FPS and produces a very rigid presentation cadence. ReflexProbe's override is intended to expose that same supported Reflex mechanism to games that otherwise pass zero.

### Commercial games are often setting Reflex every frame

An unexpected secondary finding is that multiple production engines do not merely submit Reflex settings when the option changes.

**Cyberpunk 2077** and **The Witcher 3 Remastered** repeatedly call the modern Reflex setter while the mode remains unchanged, including while Reflex is Off.

**A Plague Tale: Requiem** does the same through the old Streamline 1.x plugin path. In one captured run:

- calls `#1` through `#5860`: `mode=Off`, `frameLimitUs=0`
- calls `#5861` through `#6962`: `mode=On`, `frameLimitUs=0`
- calls `#6963` through `#14637`: `mode=On + Boost`, `frameLimitUs=0`
- all intercepted calls returned success and passed through unchanged

The call rate also strongly suggests per-frame submission rather than merely frequent polling. During stable portions of that Requiem run, Off produced approximately 240 calls per second and On + Boost approximately 224-225 calls per second, closely tracking the observed frame-rate regimes.

### RAM-resident capture and state-change display

ReflexProbe now separates capture from display. Every Reflex call successfully drained from the injected shared-memory transport is retained as a compact structured record in the controller's RAM. ReflexProbe does **not** write the capture to disk.

The GUI defaults to **State changes only**:

- the first Reflex state is displayed immediately;
- identical subsequent calls are retained and counted but produce no EDIT-control writes;
- when backend, mode, requested interval, effective interval, or result changes, the controller prints one `Previous Reflex state repeated N more times.` line and then the new state;
- when the target process exits, the final unchanged run is closed the same way, so a one-hour unchanged session produces one final repeat count rather than losing that information.

Unchecking **State changes only** rebuilds the visible log from the same RAM-resident history and shows every captured Reflex call. Rechecking it rebuilds the compressed state-change view. Ordinary controller/status messages remain interleaved with the Reflex history in either view.

The shared transport ring was enlarged from 128 to **4096 events**. This is still small (roughly a few hundred KiB of shared memory) but gives the 100 ms controller poll substantial headroom even for very high frame rates. If the transport is ever lapped anyway, ReflexProbe reports the exact number of lost calls and restarts the state-change run instead of silently pretending the capture was complete.

## Solution layout

```text
ReflexProbe.sln
    ReflexProbe      - Win32 controller / direct-launch injector
    ReflexProbe64    - x64 injected DLL / Streamline Reflex interception

Common/Protocol.h    - tiny shared-memory protocol
```

Both projects write to:

```text
bin\x64\Debug\
bin\x64\Release\
```

The current Streamline bootstrap has no third-party runtime or hooking dependency. The repository still contains the pinned MinHook submodule from the first implementation experiment, but the active Streamline path does not build or use it.

## Build prerequisites

The project intentionally follows the same Visual Studio conventions as GSyncProbe:

- Visual Studio 2022 / v143
- Windows 10 SDK
- x64 only for now
- `STREAMLINE_SDK` environment variable pointing at an NVIDIA Streamline SDK root containing `include\sl_reflex.h`

Open `ReflexProbe.sln` and build Debug or Release x64. Both projects use warning level 4 with warnings treated as errors.

## Build identity in logs

Rapid local iteration makes stale EXE/DLL pairs easy to test accidentally, so every controller run records:

- a human-readable source tag and protocol version
- the controller compile timestamp
- SHA-256 of the running `ReflexProbe.exe`
- SHA-256 of the sibling `ReflexProbe64.dll` that will be injected
- the compile timestamp reported back by the actually loaded injected DLL

The SHA-256 values are exact binary fingerprints. A protocol bump also makes incompatible stale controller/DLL pairs fail loudly instead of silently sharing the wrong structure layout.

## Streamline generations

ReflexProbe currently recognizes two Streamline Reflex integration generations.

### Modern Streamline (2.x+)

The injected DLL patches the application's import of `slGetFeatureFunction` from `sl.interposer.dll`. When the game asks Streamline for `slReflexSetOptions`, ReflexProbe keeps the genuine function pointer returned by NVIDIA and gives the game a wrapper that observes/optionally replaces only `frameLimitUs`.

This path is confirmed working in Cyberpunk 2077 and The Witcher 3 Remastered.

### Legacy Streamline (1.x)

Streamline 1.x has another layer between the game's public API and Reflex itself. NVIDIA's `sl.reflex.dll` exports `slGetPluginFunction`, and the Streamline loader asks that gateway for the plugin-private `slSetConstants` function that consumes `ReflexConstants`.

ReflexProbe therefore has two SL1 catches:

1. **Primary SL1 path:** patch only `sl.interposer.dll`'s imported `GetProcAddress`. When the interposer resolves `sl.reflex.dll!slGetPluginFunction`, ReflexProbe substitutes a tiny gateway wrapper. When that gateway is asked for `slSetConstants`, ReflexProbe substitutes the Reflex constants wrapper and changes only `frameLimitUs`.
2. **Fallback SL1 path:** if a game directly imports the older public `slSetFeatureConstants` entry point, ReflexProbe can still intercept feature ID `3` (Reflex) there.

The plugin-gateway path mirrors the actual SL1 architecture and avoids modifying NVIDIA executable code. The old `ReflexConstants` ABI is defined locally with compile-time layout checks; the project does not depend on an obsolete Streamline SDK.

This path is now confirmed working in **A Plague Tale: Requiem**, whose local `sl.reflex.dll` reports version `1.0.0.0`.

## Bootstrap workflow

1. Start `ReflexProbe.exe`.
2. Browse to a DRM-free x64 game executable.
3. Leave **Override Reflex frame limit** unchecked for the first observation run.
4. Leave **State changes only** checked for the normal readable log, or uncheck it whenever the full RAM-captured call stream is needed.
5. Choose **Launch + Inject**.
6. ReflexProbe creates the game suspended, injects `ReflexProbe64.dll`, and resumes it.
7. The injected DLL waits for a modern Streamline resolver or arms the SL1 plugin-gateway interception.
8. Every observed Reflex options/constants call is retained in controller RAM; the visible log either shows only state transitions plus repeat counts or the complete call stream.

The first MinHook implementation reached the genuine `slReflexSetOptions` implementation in Cyberpunk 2077, but Windows rejected MinHook's executable-page protection change (`MH_ERROR_MEMORY_PROTECT`). The current design stays at import-table/function-resolution boundaries instead.

For an override test, enable the checkbox and enter an FPS target before launch. The controller converts FPS to microseconds using:

```text
frameLimitUs = round(1,000,000 / FPS)
```

Example:

```text
165 FPS -> 6061 us
```

If the game requests `0 us` and the override is 165 FPS, the expected log is conceptually:

```text
requested=0 us (automatic), effective=6061 us (~164.99 FPS)
```

Changing the override while the game is running updates shared configuration immediately, but it takes effect on the next intercepted Reflex settings call.

## Known test targets and results

- **Cyberpunk 2077 (GOG):** D3D12, modern Streamline. Observation works. Runtime-loaded Reflex came from NVIDIA's NGX model cache rather than necessarily the game-directory DLL. The game repeatedly submits all Reflex states with `frameLimitUs=0`.
- **The Witcher 3 Remastered:** D3D12, modern Streamline 2.14.1-era path. Observation works. REDengine again repeatedly submits Off, On and On + Boost with `frameLimitUs=0`.
- **A Plague Tale: Requiem (GOG):** D3D12, Streamline 1.0. Observation now works through `sl.reflex!slSetConstants`. Off, On and On + Boost all submit `frameLimitUs=0`; settings are submitted at roughly frame cadence.
- **Blood of the Dawnwalker (GOG):** UE5.5.3 according to the local build; next fresh shipping-UE runtime target. Reflex integration layer is intentionally treated as unknown until observed.
- **No Man's Sky:** Vulkan + Streamline. Planned comparison target for the D3D/Vulkan split.
- **Pragmata and other launcher titles:** later, after runtime attach/watch is added.

Native NVAPI Reflex titles such as God of War (2018) are intentionally a later backend. Native Vulkan `VK_NV_low_latency2` interception is also later; Streamline comes first because it covers a large chunk of the Reflex/FG ecosystem and gives us the cleanest initial experiment.

## Deliberate limitations

The bootstrap build is not a general-purpose injector yet.

- direct launch only
- x64 only
- modern Streamline resolver interception
- Streamline 1.x plugin-gateway interception plus public-setter fallback
- capture history is RAM-only; there is intentionally no disk logger/export path
- the visible log is still a standard Win32 EDIT control with a 16 MiB text limit; the structured RAM history is separate from that display limit
- no runtime attach/watch yet
- no native NVAPI hook yet
- no native Vulkan hook yet
- no anti-cheat support

Do not use ReflexProbe with anti-cheat/protected multiplayer titles. The intended test set is DRM-free/offline/no-anti-cheat software where process injection is not fighting a protection system.

## Planned next steps

1. Observe **Blood of the Dawnwalker / UE5.5.3** and determine whether the shipping game reaches Reflex through modern Streamline or a native UE/NVAPI path.
2. Prove the actual override path in both modern and legacy D3D12 Streamline games, especially forcing `6061 us` from a game that requested zero and checking for ~165 FPS.
3. Repeat the zero-request experiment on No Man's Sky / Vulkan Streamline and compare directly against D3D behavior.
4. Add runtime **Attach** and **Watch for process** modes so normal Steam-launched games are usable without ReflexProbe knowing anything about Steam.
5. Add native NVAPI Reflex interception (`NvAPI_D3D_SetSleepMode`).
6. Add native Vulkan Reflex interception (`vkSetLatencySleepModeNV`) when a game requires it.

The architecture is intentionally boring: Win32 GUI outside the game, one injected DLL inside it, one RAM-resident event history, and one integer under the microscope.
