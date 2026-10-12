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
| **Feature selection** | **Reflex probing** enabled by default; **DLSS Frame Generation probing** initially disabled. Both are locked per acquisition. FG probing wraps modern Streamline `slDLSSGSetOptions`; interception of `slDLSSGGetState` is a separate acquisition-time checkbox, **off by default**, which returns the unmodified original function pointer when unchecked. Disabling Reflex probing leaves Reflex function pointers unwrapped. |
| **FG overrides** | **Force Auto to On** changes only an `Auto` mode request to `On`. **Override FG Off (after On)** is an experimental independent checkbox, initially off: only after a successful FG `Auto` or `On` SetOptions call with a nonzero frame count on that viewport does it replace later game `Off` requests with the last successful game's requested active mode and frame count. Failed calls never replace this saved state. A separately enabled multiplier override takes precedence for the forwarded frame count; the existing Auto-to-On override applies afterwards. **Override FG multiplier** optionally forwards a fixed 2X–6X multiplier (`numFramesToGenerate` = 1–5); the combo contains only 2X, 3X, 4X, 5X, and 6X, initially selecting 2X while its checkbox stays unchecked. The multiplier override leaves the game's mode intact (including Off), excludes Dynamic mode, and needs neither GetState interception nor a detected Streamline DLL version. **Override menu detection** forces flag `0x10`, with **Off** initially selected. **Override FG resource retention** forces flag `0x08`, with **On** initially selected. All five override checkboxes start unchecked and can change live; both flag overrides preserve unrelated bits. Unknown future option structure versions pass through unchanged. |
| **Frame-limit override** | Replaces Streamline `frameLimitUs` or NVAPI `minimumIntervalUs`; `0` is a valid override meaning no explicit interval. Can remain effective with Reflex Low Latency Off. |
| **Force Boost** | Changes a plain Reflex **On** request to **On + Boost**; does not change Off or an existing On + Boost request. |
| **Sleep-call counting** | Optional at acquisition time. Wraps supported `slReflexSleep` / `NvAPI_D3D_Sleep` calls; leaves genuine pointers untouched when unchecked. Streamline 1.x Sleep counting is not implemented. |
| **Capture modes** | **State changes** compresses identical settings calls into repeated-state counts; **Raw debug** retains the latest 131,072 raw events in a RAM ring and always displays full per-call diagnostics. |
| **Detailed FG diagnostics** | Unchecked by default. In State changes mode, unchecked shows the requested/forwarded FG mode, **requested/forwarded `numFramesToGenerate` and multiplier**, requested/forwarded fullscreen-menu detection, requested/forwarded `retainResourcesWhenOff`, and result for `slDLSSGSetOptions`. Checked restores full SetOptions lines. Raw debug always provides full information; `slDLSSGGetState` output is unchanged. The checkbox is disabled in Raw debug or when FG probing is off, but retains its checked state. |
| **Acquisition** | **Launch + Inject**, **Watch + Inject** (launcher-friendly exact executable path), and **Attach**. |
| **Diagnostics** | Backend, loaded-module versions, interception method/caller, requested/effective state, optional Sleep counts, build timestamps, protocol tag, and controller/injected DLL hashes. |
| **FG status label** | Displays the most recent **forwarded** `slDLSSGSetOptions` mode, multiplier, and menu-detection flag. GetState calls never modify it. Identical displayed values are cached to avoid redundant Win32 text updates. When intercepted, GetState output remains available in State changes and Raw debug. |

There is no driver patch, Streamline DLL replacement, shader modification, or disk capture. Frame-limit, Force Boost, and FG Auto/Off-after-On/multiplier/menu/resource-retention policies can be changed while a target runs; the Sleep-counting and FG GetState-interception choices are **locked for that acquisition**. The FPS field defaults to 158 when starting ReflexProbe, but override is **off by default**.

### FG resource retention: NVIDIA guidance and memory cost

NVIDIA's [Streamline DLSS-G programming guide, §6.4](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#64-disabling-frame-generation) says an explicit `DLSSGMode::eOff` normally releases DLSS-G's internal allocations; re-enabling may trigger allocation-related stutter. It strongly recommends `DLSSGFlags::eRetainResourcesWhenOff` (`0x08`) for **short-term** Off transitions, allowing FG to suspend without releasing that memory. For **long-term** disablement, NVIDIA instructs the application to explicitly call `slFreeResources()` after setting Off. ReflexProbe overrides only the SetOptions flag: it does **not** intercept or block `slFreeResources()`, and cannot guarantee retained allocations or control the game's full resource lifecycle.

NVIDIA's [guide, §1.1 memory-cost estimates](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#11-run-time-performance) give a **total DLSS-G memory footprint** of roughly **272 MB at 1080p**, **489 MB at 1440p**, and **725 MB at 4K**. These are representative integration-dependent VRAM estimates, **not** measured additional memory caused by enabling retention, nor a prediction of the exact amount retained during Off. The guide says the overall footprint does not change significantly between single-frame and multi-frame generation.

Forcing retention **On** is an experiment in reducing allocation-related reactivation hitches at the potential cost of memory remaining allocated while FG is Off. Forcing **Off** tests the opposite request. Neither direction requires GetState interception. The menu-detection mechanism can independently retain resources even when this flag is Off, as NVIDIA documents in [§6.5](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#65-automatically-disabling-dlss-g-in-menus).

## Requirements and building

- Visual Studio 2022 with the **v143** toolset and Windows 10 SDK
- x64 Windows target
- `STREAMLINE_SDK` environment variable pointing at an NVIDIA Streamline SDK root with `include\sl_reflex.h` and `include\sl_dlss_g.h` (this build uses the newer FG option/state fields through options v5 and state v4)

Open `ReflexProbe.sln` and build **Debug x64** or **Release x64**. Both projects treat warning-level-4 compiler warnings as errors. Output is placed in `bin\x64\Debug\` or `bin\x64\Release\`.

| Location | Purpose |
| --- | --- |
| `Controller/` | Win32 UI, process acquisition/injection, capture, and status reporting |
| `Injected/ReflexProbe64.cpp` | In-process Reflex and DLSS FG interception |
| `Common/Protocol.h` | Shared-memory event and policy protocol |
| `ThirdParty/MinHook` | Pinned historical submodule; **not currently built or used** |

## Usage

1. Start `ReflexProbe.exe` and select the **actual x64 game executable**, not a launcher wrapper. Executable-path validation is required before acquisition.
2. Choose **Enable Reflex probing**, **Enable DLSS Frame Generation probing**, or both. Select **State changes** (normal use) or **Raw debug** (short diagnostic capture). Configure Reflex frame-limit override, Force Boost, and **Count Reflex Sleep Calls** if Reflex probing is selected. For FG, optionally enable **Force Auto to On**, experimental **Override FG Off (after On)**, **Override FG multiplier** (2X–6X, initially 2X), **Override menu detection** (radio defaults to **Off**), or **Override FG resource retention** (radio defaults to **On**). Override checkboxes are **off by default**; when unchecked, the game's multiplier passes through untouched. **Intercept FG GetState calls** is independently optional and off by default. FG feature and GetState interception choices lock during Watch/acquisition; Auto, Off-after-On, multiplier, menu, and retention overrides can change while a target runs, taking effect when the game next calls SetOptions.
3. Choose **Launch + Inject** to start the executable suspended and inject before resuming it; **Watch + Inject** to arm a 10 ms exact-full-path process watch and start the game normally through its launcher; or **Attach** for an already-running executable. The FG override log records both **requested** and **forwarded** mode, multiplier, and flags; the wrapper preserves the game's input options and returns the original function result.
4. Observe requested/effective Reflex state and the **Current effective state** display. Frame-limit and Force Boost policies are live; optional Sleep counts are reported when a state ends. **Clear** is available while idle: it discards the visible history and reprints only the original ten startup diagnostics with fresh timestamps. It does not restore prior acquisition/hook messages, reattach, reinject, or write to disk.

A positive FPS entry is converted as `frameLimitUs = round(1,000,000 / FPS)` (165 FPS → 6061 µs). Entering **0** with override enabled forces literal zero, rather than the application's original interval. A nonzero override is not guaranteed to control the final presentation rate in every game.

**Watch is usually preferable for launcher-driven games and sparse startup-only Reflex setters.** Attach cannot recover earlier calls or previously cached function pointers. Raw debug is RAM-only; the shared transport has a fixed 4,096-event ring and reports overruns instead of silently hiding them.

## Observed behavior

- **An explicit limiter is independent of Low Latency mode.** God of War, Shadow Warrior 3, and Hogwarts Legacy showed effective forced limits with Reflex Off and **zero observed per-frame Sleep calls** in the relevant Off intervals. The downstream enforcement mechanism is not identified by the current probe.
- **Zero and nonzero requests must be distinguished.** Most tested games submitted `0 us`; Satisfactory submitted **16,666 µs (~60 FPS)** in its main menu and ReflexProbe overrode that hidden policy.
- **Sleep cadence varies by integration.** DOOM's settings and Sleep calls were virtually 1:1, while No Man's Sky accepted a limit with **no observed Streamline Sleep calls** and did not obey that limit.
- **Frame Generation complicates interpretation.** DOOM and Dawnwalker Reflex-only captures are consistent with Sleep calls tracking real rendered frames rather than generated output. FG SetOptions/GetState observation was subsequently validated in both games, but the Sleep-cadence inference remains unproven.
- **Interception may occur outside the main EXE.** Satisfactory resolved Streamline through a game DLL, requiring module-wide resolver interception.

### The Witcher 3 Remastered: initial SL2 FG observations (October 8, 2026)

- **Two acquisition paths verified:** the GOG release launched directly without Galaxy using **Launch + Inject**, followed by **Watch + Inject** when started through GOG Galaxy with **only FG probing enabled**. The latter intercepted both FG functions without Reflex probing. The FG plugin reported Streamline **2.14.1.0**.
- **Fixed MFG requests observed:** `slDLSSGSetOptions` returned success for **2X** (`numFramesToGenerate=1`), **3X** (`=2`), and **6X** (`=5`) with `mode=On`. `slDLSSGGetState` reported `numFramesToGenerateMax=5` (6X maximum) and Dynamic MFG support, but the game submitted no `mode=Dynamic` requests in these tests.
- **Menus are explicitly controlled by the game:** transitions into and out of menus produced `mode=Off` / `mode=On` requests. The fullscreen-menu-detection flag remained **unset** (`flags=0x00000000`) in observed SetOptions calls. UI recomposition was **Yes** for captured FG-On configurations and **No** for FG-Off configurations. Changes to the selected multiplier became visible when the game submitted updated options after leaving the menus.
- **Submission is frequent:** tens of thousands of otherwise identical FG SetOptions calls were reduced to repeated-state counts. FG SetOptions, FG GetState, and Reflex histories remained independent; per-call presented-frame counts were not treated as persistent GetState changes.
- **Presentation-rate measurement remains separate:** 6X is a successfully submitted multiplier, not a measurement of original rendered FPS. A monitor indication around 225 is insufficient to establish either actual displayed FPS or base rendering cadence; the next comparison should use an overlay that distinguishes rendered FPS from DLSS-generated output (such as Steam's separate FPS / DLSS FPS readings).

### Experimental MFG multiplier limits: Dawnwalker and Indiana Jones (October 11, 2026)

On an RTX 5090 with a reported NVIDIA 610.47 driver, ReflexProbe's live **2X–6X fixed multiplier override** was tested against two DRM-free GOG games. The game-originated `slDLSSGSetOptions` calls were modified without altering the games' own selected 2X multiplier or requiring `slDLSSGGetState` interception for the override.

| Game | Active components | Successful overrides | Rejected requests and observed fallback |
| --- | --- | --- | --- |
| **Blood of the Dawnwalker** | Game `sl.interposer.dll` **2.7.30.0**; NGX `sl_dlss_g` **2.14.0.0** | **2X–4X**; expected generated-inclusive rates additionally confirmed through Steam's overlay when launched as a non-Steam game | **6X** explicitly returned `38`; tester also observed **5X** disengaging FG (not separately captured in the short log). Unsupported requests visibly stopped FG; restoring 4X recovered. |
| **Indiana Jones and the Great Circle** | Game `sl.interposer.dll` **2.7.32.0**; NGX `sl_dlss_g` **2.14.0.0** | **2X–4X**; optional GetState Raw capture reported **2/3/4 presented frames** and **`numFramesToGenerateMax=3` (4X max)** | **5X and 6X** explicitly returned `38`; FG **continued at the previously accepted 4X**, also reflected in the game's own top-right performance metrics. |

Here `38` is Streamline's **`sl::Result::eErrorInvalidState`**, not a promise that FG will stop. Indiana Jones supplied direct GetState evidence of a 4X runtime maximum; **Dawnwalker did not have GetState interception enabled**, so its precise reported maximum is unknown. Same-version NGX plugin files do not, by themselves, establish the cause or guarantee identical fallback behavior. GetState's per-call presented-frame count is **transient telemetry**, intentionally omitted from State changes comparisons; its first call and persistent capability fields are still logged. See **[per-game evidence and caveats](docs/TestFindings.md#per-game-results)**.

## Tested games

These are observed results from specific tested builds, not a universal compatibility guarantee. Detailed evidence and caveats are in **[Test findings](docs/TestFindings.md#per-game-results)**.

| Game / test target | Renderer / backend | Distinguishing observation |
| --- | --- | --- |
| GSyncProbe | D3D11 / D3D12 / Vulkan | Controlled 240 Hz baseline and 165/60/30 FPS override validation. |
| Cyberpunk 2077 | D3D12 / Streamline 2.x+ | Frequent zero-interval Reflex settings submissions. |
| The Witcher 3 Remastered | D3D12 / Streamline 2.14.1 | Reflex probing plus validated standalone/GOG Galaxy FG-only acquisition; observed 2X, 3X, 6X and explicit menu Off/On requests with detector flag unset. |
| A Plague Tale: Requiem | D3D12 / Streamline 1.x | Legacy plugin gateway; injected Force Boost has a performance caveat. |
| Blood of the Dawnwalker | D3D12 / Streamline 2.x+ | Fixed override 2X–4X works; 6X returns invalid state and FG disengages; 5X failure user-observed. |
| Indiana Jones and the Great Circle | Vulkan / Streamline 2.x+ | GetState reports 4X max; 2X–4X work; rejected 5X/6X requests leave 4X FG operating. |
| No Man's Sky | Vulkan / Streamline 2.x+ | Override accepted but presentation did not obey; zero observed Sleep calls. |
| God of War (2018) | D3D11 / native NVAPI | Explicit limit works even when Reflex Off records no Sleep calls. |
| DOOM: The Dark Ages | Vulkan / Streamline 2.x+ | Game CVar maps to Reflex interval; PT/FG introduces pacing transitions. |
| Shadow Warrior 3 | Native NVAPI D3D | Launcher requires exact-path Watch; Off has zero Sleeps but working limit. |
| Satisfactory | Streamline 2.x+ | Module-DLL resolver; hidden main-menu 60 FPS Reflex limiter. |
| Hogwarts Legacy | D3D12 / Streamline 2.x+ | Late UE4 to MFG; independent game cap, zero Sleeps with Reflex Off. |
| Pragmata | D3D12 / Streamline 2.x+ | Sparse settings, ongoing Sleep cadence; menu/FG behavior not yet resolved. |

## Interception architecture

**Modern Streamline:** intercept normal `slGetFeatureFunction` imports and selected `GetProcAddress` IAT lookups in loaded non-system modules, then wrap selected Reflex functions (`slReflexSetOptions`, optional `slReflexSleep`) and/or DLSS FG functions (`slDLSSGSetOptions`, optionally `slDLSSGGetState`). Without FG overrides, SetOptions forwards unchanged. With overrides, it copies only recognized-version caller fields, changes the selected mode/flag bits, forwards the modified structure, and logs both original and forwarded values. With GetState interception disabled, the original function pointer is returned without a wrapper. The scan preserves Streamline's legacy gateway and skips already-hooked resolver slots.

**Legacy Streamline 1.x:** intercept the `sl.reflex.dll` plugin gateway `slGetPluginFunction("slSetConstants")`, with an older `slSetFeatureConstants` fallback. Its Sleep path is not currently counted.

**Native NVAPI D3D:** intercept the `nvapi_QueryInterface` / direct `NvAPI_D3D_SetSleepMode` resolution seam; optionally count `NvAPI_D3D_Sleep`. This does not patch code inside `nvapi64.dll`.

The project uses API-boundary pointer substitution rather than an active generic detour library. See **[Test findings: Resolver discovery and regression](docs/TestFindings.md#resolver-discovery-and-regression)** for the observed caller-module differences and regressions.

## Compatibility, limitations, and next steps

ReflexProbe is **x64-only**. It makes no distinction between offline and online games, DRM-free and DRM-protected releases, or targets with and without anti-cheat. Whether injection and API interception work depends on the particular game's integration and protections; DRM and anti-cheat implementations vary, may interfere with these techniques, and may impose restrictions or consequences for using injected software. ReflexProbe is provided **as-is**, without any compatibility guarantee. Users decide where and how to use it.

Native Vulkan `VK_NV_low_latency2` interception and Streamline 1.x Sleep counting are **not yet implemented**. The visible Win32 log is bounded, Attach can miss already-cached pointers, and successful Reflex settings interception does not prove an effective presentation cap.

Modern Streamline FG probing uses the existing module-wide resolver and may be selected without Reflex probing. State changes uses separate Reflex, FG SetOptions, and FG GetState histories, keyed by viewport for FG. FG GetState changes compare captured persistent **returned** fields, not caller-supplied query options or transient per-call presentation counts. Query options and presentation counts remain available in Raw debug. Fullscreen-menu detection is recorded from the game's options flag; the runtime does not expose a dedicated "detector triggered" status. FG interception depends on the game resolving the observed functions after the probe is armed, so absent calls are not proof of absent FG activity.

Next: test forced FG resource retention (On and Off) across short-term disable/re-enable transitions, noting stutters and VRAM consequences. Test the experimental 2X–6X multiplier override across different Streamline and GPU combinations: the runtime may ignore, reject, or limit a requested multiplier, and SetOptions success does not establish the number of generated frames actually presented. No GetState interception, Streamline file-version gate, or synthetic SetOptions submission is used by the override. Consider direct late-attach fallback only if confirmed necessary.

**FG presentation filtering:** State changes always compares complete captured FG states and preserves each intercepted SetOptions call number. In Normal FG mode, changes confined to hidden fields are not logged; genuine full-state repetitions accumulate across those suppressed state changes until the next visible state or target exit. The Normal summary therefore says *repeated calls since last displayed state*, not that one complete state remained identical. Switching Detailed FG diagnostics while running flushes pending repetitions under the old view and prints a current FG SetOptions snapshot under the new view without issuing a new Streamline call, resetting the complete-state tracker, or changing the API call sequence. Switching Raw debug and State changes likewise retains the latest observed FG state and establishes a new display baseline. A separate Reflex-detail view remains a possible future refinement.

Technical experiments and implementation history live in **[docs/TestFindings.md](docs/TestFindings.md)**.
