# ReflexProbe

ReflexProbe is a Windows x64 tool for **observing and overriding NVIDIA Reflex and DLSS Frame Generation requests** made by games. A small Win32 controller and injected DLL report the game's **requested → forwarded** settings and API results, and optionally modify what is submitted to NVIDIA's runtime.

**Reflex probing** supports modern Streamline (2.x+), legacy Streamline 1.x, and native NVAPI D3D. It can override an explicit frame limit, force Reflex Boost, and optionally count application-level Reflex Sleep calls.

**DLSS Frame Generation probing** supports modern Streamline, including Multi Frame Generation (MFG). It can inspect FG configuration and optional state queries, override fixed 2X–6X multipliers and selected flags, and experimentally **force FG On when a game requests Off**. The Off override can wait for an established successful active configuration or try immediately. **Accepted calls are not proof of generated output:** Pragmata continued FG when Off was overridden, but several other games did not.

ReflexProbe has **no overlay and writes no capture data to disk**. Its results are game- and integration-dependent; see [Test Findings](docs/TestFindings.md) for measurements, successes, failures, and limitations.

## Why ReflexProbe exists

With **Reflex enabled, VSync on, and G-SYNC active**, testing at 240 Hz revealed a repeatable difference between graphics APIs:

| Rendering API | Observed behavior at 240 Hz |
| --- | --- |
| D3D11 | Approximately **225 FPS** |
| D3D12 | Approximately **225 FPS** |
| Vulkan | Approximately **240 FPS**; no equivalent automatic below-refresh cap |

In these tests the games generally submitted a **zero** explicit Reflex frame interval. That means the D3D below-refresh behavior was not simply the game asking for 225 FPS. The empirical D3D rule fits `cap(R) ≈ 3600R / (3600 + R)`; it is an observation, **not** a claim about NVIDIA's internal implementation.

The original question was: **what Reflex settings does a game submit, and what happens if we change them?** ReflexProbe intercepts that boundary. Vulkan's lack of the same *automatic* cap does **not** mean it cannot honor an *explicit* Reflex limit; DOOM: The Dark Ages does. The same approach now asks whether overriding a game's DLSS FG Off request actually keeps generation running.

Full measurements, exceptions, and per-game evidence: **[Test findings](docs/TestFindings.md)**.

## Features

### Acquisition and diagnostics

| Capability | Behavior |
| --- | --- |
| **Select probes** | Reflex probing starts enabled; DLSS FG probing starts disabled. Choose either or both before acquisition; probe selection is fixed for that target. |
| **Acquisition** | **Launch + Inject**, exact-path **Watch + Inject** for launcher-driven games, and **Attach**. |
| **Capture modes** | **State changes** compresses repeated calls with numbered-call references. **Raw debug** retains the last **131,072** raw events in RAM. Reflex, FG SetOptions and FG GetState histories are separate. |
| **Diagnostics** | Original **requested → forwarded** values, results, module/backend information, repeated-call and Sleep-count references, build timestamps, protocol tag, and EXE/DLL SHA-256 hashes. |
| **Live override controls** | Reflex frame-limit/Boost and FG Auto/Off/multiplier/menu/retention policies can change during a session; selected interception options are acquisition-time only. |

### NVIDIA Reflex probing

| Capability | Behavior |
| --- | --- |
| **Reflex observation** | Original and forwarded **Off / On / On + Boost** settings through modern Streamline, legacy Streamline 1.x, and native NVAPI D3D. |
| **Frame-limit override** | Substitute a user-selected FPS limit via `frameLimitUs` or NVAPI `minimumIntervalUs`; **0** forces no explicit interval. The configured limit can remain effective even when Reflex Low Latency is Off. |
| **Force Boost** | Forward ordinary **On** as **On + Boost**; preserve Off and already-Boosted requests. |
| **Reflex Sleep counting** | Optional acquisition-time wrapping of supported Streamline/NVAPI Sleep calls; **not implemented for Streamline 1.x**. |

### DLSS Frame Generation probing

| Capability | Behavior |
| --- | --- |
| **FG SetOptions** | Observe `slDLSSGSetOptions` original/forwarded mode, frame count, multiplier, flags, and result. Independent of Reflex probing. |
| **FG GetState** | Optional acquisition-time interception of `slDLSSGGetState` for capability/status and frame-presentation telemetry. Off by default; when off the genuine function pointer is preserved. |
| **Force Auto to On** | Forward an `Auto` request as `On`. |
| **Override FG Off** | Experimental game-requested **Off → active** override: **After success** (default) or **Always**. The checkbox starts unchecked. [History rules below](#experimental-fg-off-override). |
| **Override FG multiplier** | Fixed **2X–6X** (generated count 1–5); the override takes precedence over the game's or stored count. Checkbox off and choice **2X** by default. Does not independently enable FG or change Dynamic mode. |
| **Override menu detection** | Force fullscreen-menu-detection flag **0x10**; the default choice is **Off** but its checkbox starts unchecked. |
| **Override FG resource retention** | Force flag **0x08**; the default choice is **On** but its checkbox starts unchecked. Other flag bits are preserved. |
| **Detailed FG diagnostics** | Compact or full SetOptions presentation in State changes mode; Raw debug is always detailed. GetState comparison excludes transient presentation counts. |
| **FG status label** | Displays last **forwarded** FG mode, multiplier and menu flag; GetState calls do not change it. |

All override checkboxes start **unchecked**. Unknown future options structure versions pass through unchanged. Setter `result=0` indicates the forwarded API request was accepted, **not** that frames were actually generated.

### Experimental FG Off override

**After success** waits for a successful active SetOptions call on the same viewport. **Always** can also force the **first** eligible Off request to On using the game's current frame count (even zero), subject to the independent multiplier override. A successful first forced Off → On can establish history.

Once history exists, **both modes reuse the last successful active configuration** for subsequent Off requests. Successful native Auto/On calls refresh it; later Off calls and failed calls do not. History remembers the **game's original mode and `numFramesToGenerate`**, including zero, rather than the override's selected multiplier. Other enabled overrides take precedence on forwarding; there is **no fallback multiplier, synthetic call, or retry**.

This is an experimental interception technique, **not a universal menu workaround**: [Pragmata kept FG running](docs/TestFindings.md#pragmata-steam) with Off overridden, whereas [other games did not](docs/TestFindings.md#fg-off-override-cross-title-outcomes). NVIDIA [documents](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#64-disabling-frame-generation) that disabling FG can release resources; ReflexProbe does **not** intercept `slFreeResources()`. See [resource-retention and memory caveats](docs/TestFindings.md#fg-resource-retention-and-its-limits).

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

1. Start `ReflexProbe.exe` and select the **actual x64 game executable**, not a launcher wrapper.
2. Enable **Reflex probing**, **DLSS Frame Generation probing**, or both. Configure the desired overrides and optional Sleep counting / FG GetState interception. The FG Off dropdown defaults to **After success**; all override checkboxes begin unchecked.
3. Use **Launch + Inject**, **Watch + Inject**, or **Attach**. Watch is usually best for launcher-driven games or startup-only setters; Attach cannot recover calls already made or function pointers already cached. Probe selection, Sleep counting, and FG GetState interception are locked for the acquisition; applicable override policies can change live and affect the next game SetOptions call.
4. Observe the **requested → forwarded** values and API result codes. **State changes** is the compact default; **Raw debug** records each event in a bounded RAM ring. **Clear**, available while idle, resets the visible history, frees raw capture, and reprints startup diagnostics without disk logging.

Reflex FPS targets use `frameLimitUs = round(1,000,000 / FPS)` (165 FPS → 6061 µs); enabling a literal **0** forces zero. The FPS field begins at **158** with its override unchecked. A successful setter does not necessarily govern presentation.

## Observed behavior

- **Explicit Reflex limits can outlast Reflex mode:** God of War, Shadow Warrior 3, and Hogwarts Legacy obeyed forced limits with Reflex **Off** and **zero observed Sleep calls** in those intervals. The downstream mechanism remains unidentified.
- **Hidden limits exist:** Satisfactory requested **16,666 µs (~60 FPS)** in its main menu, which ReflexProbe overrode.
- **Forced FG continuation can work:** Pragmata kept FG running through game-requested Off transitions forwarded as On; Steam's overlay showed generated output continuing without observed visual artifacts.
- **It can also fail:** Deadzone: Rogue and Satisfactory stopped FG despite accepted overridden requests. **Cyberpunk 2077 and The Witcher 3 Remastered did not retain FG in their commonly used gameplay menus** with Off overridden.
- **Sometimes there is nothing to override:** Dawnwalker and The Talos Principle 2 have tested menus where the game already keeps FG running, independently of ReflexProbe's Off override.
- **MFG multiplier overrides are a separate capability:** Satisfactory and other games accepted multipliers beyond exposed menu choices; unsupported requests can be rejected or disengage FG, with differing fallback behavior.
- **API acceptance is not measured output:** No Man's Sky accepted a Reflex interval that did not govern presentation. A successful FG SetOptions call similarly does not prove continued generation.

See [FG continuation comparisons](docs/TestFindings.md#fg-off-override-cross-title-outcomes), [detailed game tests](docs/TestFindings.md#per-game-results), and [Requiem's PresentMon evidence](docs/TestFindings.md#a-plague-tale-requiem-gog).

## Tested games

Observations below are specific to tested builds, not guarantees of compatibility. Detailed evidence is in [TestFindings.md](docs/TestFindings.md#per-game-results).

| Game / test target | Renderer / backend | Distinguishing observation |
| --- | --- | --- |
| GSyncProbe | D3D11 / D3D12 / Vulkan | Controlled 240 Hz and 165/60/30 FPS override measurements. |
| Cyberpunk 2077 | D3D12 / Streamline | FG Off override did **not** preserve generation in normal gameplay menus. |
| The Witcher 3 Remastered | D3D12 / Streamline | FG probing showed 2X/3X/6X submissions; Off override did **not** preserve FG in normal gameplay menus. |
| A Plague Tale: Requiem | D3D12 / Streamline 1.x | PresentMon captured ~158 → ~30 FPS collapse from forced Boost; choosing native Boost restored it. |
| Blood of the Dawnwalker | D3D12 / Streamline | Fixed 2X–4X overrides work; FG already stays on in tested menus. |
| Indiana Jones and the Great Circle | Vulkan / Streamline | GetState reports 4X maximum; rejected 5X/6X retained prior working 4X. |
| No Man's Sky | Vulkan / Streamline | Accepted explicit Reflex limit did not control presentation. |
| God of War (2018) | D3D11 / native NVAPI | Working explicit limiter with Reflex Off and zero observed Sleeps. |
| DOOM: The Dark Ages | Vulkan / Streamline | Reflex CVar and PT/FG pacing transitions. |
| Shadow Warrior 3 | Native NVAPI D3D | Exact-path Watch acquisition; working limiter with Reflex Off. |
| Satisfactory | Streamline | Hidden menu 60 FPS limiter; multiplier override works; Off override did not preserve FG. |
| Hogwarts Legacy | D3D12 / Streamline | Late UE4 MFG retrofit and working limiter with Reflex Off. |
| Pragmata | D3D12 / Streamline | **FG continued operating through overridden game-requested Off calls.** |
| Deadzone: Rogue | Modern Streamline | **FG stopped despite Off → On returning success.** |
| The Talos Principle 2 | UE5 / Streamline | Explicit limiter works; usual menus keep FG enabled without the Off override. |

## Interception architecture

**Modern Streamline:** intercept normal `slGetFeatureFunction` imports and selected `GetProcAddress` IAT lookups in loaded non-system modules, then wrap selected Reflex functions (`slReflexSetOptions`, optional `slReflexSleep`) and/or DLSS FG functions (`slDLSSGSetOptions`, optionally `slDLSSGGetState`). Without FG overrides, SetOptions forwards unchanged. With overrides, it copies only recognized-version caller fields, changes the selected mode/flag bits, forwards the modified structure, and logs both original and forwarded values. With GetState interception disabled, the original function pointer is returned without a wrapper. The scan preserves Streamline's legacy gateway and skips already-hooked resolver slots.

**Legacy Streamline 1.x:** intercept the `sl.reflex.dll` plugin gateway `slGetPluginFunction("slSetConstants")`, with an older `slSetFeatureConstants` fallback. Its Sleep path is not currently counted.

**Native NVAPI D3D:** intercept the `nvapi_QueryInterface` / direct `NvAPI_D3D_SetSleepMode` resolution seam; optionally count `NvAPI_D3D_Sleep`. This does not patch code inside `nvapi64.dll`.

The project uses API-boundary pointer substitution rather than an active generic detour library. See **[Test findings: Resolver discovery and regression](docs/TestFindings.md#resolver-discovery-and-regression)** for the observed caller-module differences and regressions.

## Compatibility, limitations, and next steps

ReflexProbe is **x64-only**. It makes no distinction between offline and online games, DRM-free and DRM-protected releases, or targets with and without anti-cheat. Whether injection and API interception work depends on the particular game's integration and protections; DRM and anti-cheat implementations vary, may interfere with these techniques, and may impose restrictions or consequences for using injected software. ReflexProbe is provided **as-is**, without any compatibility guarantee. Users decide where and how to use it.

Native Vulkan `VK_NV_low_latency2` interception and Streamline 1.x Sleep counting are **not yet implemented**. The visible Win32 log is bounded, Attach can miss already-cached pointers, and successful Reflex settings interception does not prove an effective presentation cap.

Modern Streamline FG probing uses the existing module-wide resolver and can be selected without Reflex probing. State changes uses separate Reflex, FG SetOptions and FG GetState histories, keyed by viewport for FG. GetState comparisons use persistent returned fields and omit transient per-call presentation counts; Raw debug retains those values.

**FG presentation filtering:** Compact State changes mode still compares the complete captured FG state, suppressing changes only to fields hidden from the compact view. Repetition summaries reference the previous displayed API call number (plus viewport for FG), not necessarily identical full states throughout. Changing detailed FG display while running flushes pending counts and prints a snapshot without issuing another Streamline call.

Further experiments concern why FG Off → On succeeds in Pragmata but fails to keep generation running in other integrations, along with the resource lifecycle and actual presentation behavior beyond accepted API calls. See [Test Findings](docs/TestFindings.md).
