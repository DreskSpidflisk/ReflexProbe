# ReflexProbe

ReflexProbe is a Windows x64 tool for observing and overriding the NVIDIA Reflex settings submitted by games. It supports modern Streamline (2.x+), legacy Streamline 1.x, and native NVAPI D3D integrations through one injected DLL and a small Win32 controller.

It displays **requested versus effective** Reflex modes and frame-limit intervals, can substitute an explicit FPS limit or force Boost, and can optionally count application-level Reflex Sleep calls. It has no overlay and writes **no capture data to disk**.

## Why ReflexProbe exists

With **Reflex enabled, VSync on, and G-SYNC active**, testing at 240 Hz revealed a repeatable difference between graphics APIs:

| Rendering API | Observed behavior at 240 Hz |
| --- | --- |
| D3D11 | Approximately **225 FPS** |
| D3D12 | Approximately **225 FPS** |
| Vulkan | Approximately **240 FPS**; no equivalent automatic below-refresh cap |

In these tests the games generally submitted a **zero** explicit Reflex frame interval. That means the D3D below-refresh behavior was not simply the game asking for 225 FPS. The empirical D3D rule fits `cap(R) ≈ 3600R / (3600 + R)`; it is an observation, **not** a claim about NVIDIA's internal implementation.

The resulting question is straightforward: **what settings does the application actually send to Reflex, and what happens if we change them?** ReflexProbe intercepts that boundary. Vulkan's lack of the same *automatic* cap does **not** mean that Vulkan cannot honor an *explicit* Reflex limit; DOOM: The Dark Ages does, for example.

Full measurements, exceptions, and per-game evidence: **[Test findings](docs/TestFindings.md)**.

## Features

| Capability | Behavior |
| --- | --- |
| **Feature selection** | **Reflex probing** enabled by default; **DLSS Frame Generation probing** initially disabled. Both are selectable and locked per acquisition. In this UI-first revision, selecting FG does **not yet** wrap or observe FG functions; its observation/override controls are pending. Disabling Reflex probing leaves Reflex function pointers unwrapped. |
| **Frame-limit override** | Replaces Streamline `frameLimitUs` or NVAPI `minimumIntervalUs`; `0` is a valid override meaning no explicit interval. Can remain effective with Reflex Low Latency Off. |
| **Force Boost** | Changes a plain Reflex **On** request to **On + Boost**; does not change Off or an existing On + Boost request. |
| **Sleep-call counting** | Optional at acquisition time. Wraps supported `slReflexSleep` / `NvAPI_D3D_Sleep` calls; leaves genuine pointers untouched when unchecked. Streamline 1.x Sleep counting is not implemented. |
| **Capture modes** | **State changes** compresses identical settings calls into repeated-state counts; **Raw debug** retains the latest 131,072 raw events in a RAM ring. |
| **Acquisition** | **Launch + Inject**, **Watch + Inject** (launcher-friendly exact executable path), and **Attach**. |
| **Diagnostics** | Backend, loaded-module versions, interception method/caller, requested/effective state, optional Sleep counts, build timestamps, protocol tag, and controller/injected DLL hashes. |

There is no driver patch, Streamline DLL replacement, shader modification, or disk capture. Frame-limit and Force Boost policies can be changed while a target runs; the Sleep-counting choice is **locked for that acquisition**. The FPS field defaults to 158 when starting ReflexProbe, but override is **off by default**.

## Requirements and building

- Visual Studio 2022 with the **v143** toolset and Windows 10 SDK
- x64 Windows target
- `STREAMLINE_SDK` environment variable pointing at an NVIDIA Streamline SDK root with `include\sl_reflex.h`

Open `ReflexProbe.sln` and build **Debug x64** or **Release x64**. Both projects treat warning-level-4 compiler warnings as errors. Output is placed in `bin\x64\Debug\` or `bin\x64\Release\`.

| Location | Purpose |
| --- | --- |
| `Controller/` | Win32 UI, process acquisition/injection, capture, and status reporting |
| `Injected/ReflexProbe64.cpp` | In-process Reflex interception |
| `Common/Protocol.h` | Shared-memory event and policy protocol |
| `ThirdParty/MinHook` | Pinned historical submodule; **not currently built or used** |

## Usage

1. Start `ReflexProbe.exe` and select the **actual x64 game executable**, not a launcher wrapper. Executable-path validation is required before acquisition.
2. Choose **Enable Reflex probing**, **Enable DLSS Frame Generation probing**, or both. Select **State changes** (normal use) or **Raw debug** (short diagnostic capture). Configure Reflex frame-limit override, Force Boost, and **Count Reflex Sleep Calls** if Reflex probing is selected. The feature checkboxes lock when Watch is armed or a target is acquired; the FG observer is not implemented yet.
3. Choose **Launch + Inject** to start the executable suspended and inject before resuming it; **Watch + Inject** to arm a 10 ms exact-full-path process watch and start the game normally through its launcher; or **Attach** for an already-running executable.
4. Observe requested/effective Reflex state and the **Current effective state** display. Frame-limit and Force Boost policies are live; optional Sleep counts are reported when a state ends. **Clear** is available while idle.

A positive FPS entry is converted as `frameLimitUs = round(1,000,000 / FPS)` (165 FPS → 6061 µs). Entering **0** with override enabled forces literal zero, rather than the application's original interval. A nonzero override is not guaranteed to control the final presentation rate in every game.

**Watch is usually preferable for launcher-driven games and sparse startup-only Reflex setters.** Attach cannot recover earlier calls or previously cached function pointers. Raw debug is RAM-only; the shared transport has a fixed 4,096-event ring and reports overruns instead of silently hiding them.

## Observed behavior

- **An explicit limiter is independent of Low Latency mode.** God of War, Shadow Warrior 3, and Hogwarts Legacy showed effective forced limits with Reflex Off and **zero observed per-frame Sleep calls** in the relevant Off intervals. The downstream enforcement mechanism is not identified by the current probe.
- **Zero and nonzero requests must be distinguished.** Most tested games submitted `0 us`; Satisfactory submitted **16,666 µs (~60 FPS)** in its main menu and ReflexProbe overrode that hidden policy.
- **Sleep cadence varies by integration.** DOOM's settings and Sleep calls were virtually 1:1, while No Man's Sky accepted a limit with **no observed Streamline Sleep calls** and did not obey that limit.
- **Frame Generation complicates interpretation.** DOOM and Dawnwalker captures are consistent with Sleep calls tracking real rendered frames rather than generated output; directly observing DLSS-G state is planned, so this remains an inference.
- **Interception may occur outside the main EXE.** Satisfactory resolved Streamline through a game DLL, requiring module-wide resolver interception.

## Tested games

These are observed results from specific tested builds, not a universal compatibility guarantee. Detailed evidence and caveats are in **[Test findings](docs/TestFindings.md#per-game-results)**.

| Game / test target | Renderer / backend | Distinguishing observation |
| --- | --- | --- |
| GSyncProbe | D3D11 / D3D12 / Vulkan | Controlled 240 Hz baseline and 165/60/30 FPS override validation. |
| Cyberpunk 2077 | D3D12 / Streamline 2.x+ | Frequent zero-interval Reflex settings submissions. |
| The Witcher 3 Remastered | D3D12 / Streamline 2.x+ | Repeated zero-interval requests across Reflex modes. |
| A Plague Tale: Requiem | D3D12 / Streamline 1.x | Legacy plugin gateway; injected Force Boost has a performance caveat. |
| Blood of the Dawnwalker | D3D12 / Streamline 2.x+ | Sparse startup states; dynamic resolution and GOG Watch verified. |
| Indiana Jones and the Great Circle | Vulkan / Streamline 2.x+ | DLSS-gated Reflex/FG; setter and Sleep calls nearly 1:1. |
| No Man's Sky | Vulkan / Streamline 2.x+ | Override accepted but presentation did not obey; zero observed Sleep calls. |
| God of War (2018) | D3D11 / native NVAPI | Explicit limit works even when Reflex Off records no Sleep calls. |
| DOOM: The Dark Ages | Vulkan / Streamline 2.x+ | Game CVar maps to Reflex interval; PT/FG introduces pacing transitions. |
| Shadow Warrior 3 | Native NVAPI D3D | Launcher requires exact-path Watch; Off has zero Sleeps but working limit. |
| Satisfactory | Streamline 2.x+ | Module-DLL resolver; hidden main-menu 60 FPS Reflex limiter. |
| Hogwarts Legacy | D3D12 / Streamline 2.x+ | Late UE4 to MFG; independent game cap, zero Sleeps with Reflex Off. |
| Pragmata | D3D12 / Streamline 2.x+ | Sparse settings, ongoing Sleep cadence; menu/FG behavior not yet resolved. |

## Interception architecture

**Modern Streamline:** intercept normal `slGetFeatureFunction` imports and selected `GetProcAddress` IAT lookups in loaded non-system modules, then wrap `slReflexSetOptions` and optionally `slReflexSleep`. The scan preserves Streamline's legacy gateway and skips already-hooked resolver slots.

**Legacy Streamline 1.x:** intercept the `sl.reflex.dll` plugin gateway `slGetPluginFunction("slSetConstants")`, with an older `slSetFeatureConstants` fallback. Its Sleep path is not currently counted.

**Native NVAPI D3D:** intercept the `nvapi_QueryInterface` / direct `NvAPI_D3D_SetSleepMode` resolution seam; optionally count `NvAPI_D3D_Sleep`. This does not patch code inside `nvapi64.dll`.

The project uses API-boundary pointer substitution rather than an active generic detour library. See **[Test findings: Resolver discovery and regression](docs/TestFindings.md#resolver-discovery-and-regression)** for the observed caller-module differences and regressions.

## Limitations and next steps

ReflexProbe is **x64-only** and intended for offline, non-anti-cheat test software. It does not support anti-cheat/protected multiplayer environments, native Vulkan `VK_NV_low_latency2` interception, or Streamline 1.x Sleep counting. The visible Win32 log is bounded, Attach can miss already-cached pointers, and successful Reflex settings interception does not prove an effective presentation cap.

The two feature groups are present ahead of FG interception. Selecting only FG currently injects the observer infrastructure but produces no FG function events; this is **not** evidence that a game omitted FG calls.

Next: extend the modern resolver architecture to observe **DLSS Frame Generation** through `slDLSSGSetOptions` / `slDLSSGGetState`, then investigate multiplier state and menu suppression in Satisfactory, Hogwarts Legacy, and DOOM. Legacy/pre-MFG FG must remain capability-aware rather than assuming modern multipliers. Consider a direct late-attach fallback only if confirmed necessary.

Technical experiments and implementation history live in **[docs/TestFindings.md](docs/TestFindings.md)**.
