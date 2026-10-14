# ReflexProbe — Test Findings

This document records the runtime observations, integration differences, and acquisition/regression findings behind [ReflexProbe](../README.md). It is an **empirical test record** for specific game builds and driver conditions, not a statement about every version of those games or undocumented NVIDIA internals. Unless stated otherwise, requested/forwarded fields are captured from intercepted Reflex or FG calls. **Forwarded settings, accepted API results, GetState telemetry, and independently observed frame generation are distinct kinds of evidence.**

**Terminology:** `frameLimitUs=0` means *no explicit Reflex frame-limit interval*, not “Reflex disabled” and not a request for any particular implicit cap. With an active override, **requested** is the game's original setting and **forwarded** is the value passed onward after ReflexProbe policy; this does not assert that the runtime realized those settings. `6329 µs` targets approximately **158.003 FPS**.

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

In one DOOM PT + 2X-FG interval, **2,720 Sleeps over ~35 seconds (~77.7/s)** accompanied approximately **158 FPS displayed output**; Dawnwalker has similar evidence. This is consistent with Sleep being called on *real rendered frames* instead of generated frames, but these earlier Reflex-only cadence measurements remain inferential even though ReflexProbe now has separate DLSS-G SetOptions and optional GetState interception.

### FG Off override: cross-title outcomes

Modern Streamline FG probing can convert a game's `slDLSSGSetOptions` `Off` request into an active request. **After success** waits for a previous successful active configuration on that viewport. **Always** also tries immediately, forwarding the current game-submitted frame count (even zero) with our enabled overrides. A successful first forced Off → On may establish history. Once history exists, **both modes reuse the last successful active configuration** on later Off requests. Successful native Auto/On calls refresh it; subsequent Off calls and failed calls never replace it. History stores the game's **original mode and `numFramesToGenerate`**, including zero, not the substituted multiplier. There is no automatic 2X fallback or retry.

**SetOptions `result=0` is not proof that Frame Generation continues.** Observations:

| Game | FG Off override outcome | Qualification |
| --- | --- | --- |
| **Pragmata** | **Success:** Off requests forwarded as On returned `result=0`; Steam's overlay showed continued generation without visible artifacts. Aggressive initial activation also reportedly succeeded. | Verified positive case with overlay observation, specific to this integration. |
| **Resident Evil Requiem** | **Success:** with **After success** enabled, tested menu/map/inventory/video-playing transitions retained FG when game `Off` was forwarded as `On`; disabling and re-enabling the override restored direct Off pass-through and forced-On forwarding respectively. | Steam D3D12/RE Engine build; game operation verified by the tester, with successful SetOptions calls and GetState reporting a **4X maximum**. Only tested menus are covered; **Always** was not tested. |
| **Hogwarts Legacy** | **Success:** overriding FG Off to On kept DLSS FG running throughout the tested menus. | User-observed success in a heavily customized UE4 integration; not a general statement about UE4 games or proof that the aggressive **Always** policy was needed. |
| **Deadzone: Rogue** | **Failure despite `result=0`:** FG stopped; GetState presented-frame count dropped from three to one. | An accepted request did not preserve generated output. |
| **Satisfactory** | **Failure:** forced On did not keep FG running; the game also changed Reflex state. | Cannot attribute the result to one mechanism alone. |
| **Cyberpunk 2077** | **Failure in commonly used gameplay menus.** FG did not continue under forced On. | Not an exhaustive test of every possible scene or menu. |
| **The Witcher 3 Remastered** | **Failure in commonly used gameplay menus.** FG did not continue under forced On. | Separate from its valid MFG submissions. |
| **Blood of the Dawnwalker** | Normal tested menu navigation already kept FG active. | Not an Off-override success. |
| **The Talos Principle 2** | Menus ordinarily kept FG active with the Off override disabled. | Not an Off-override success. |

The intercepted boundary is a game-requested FG configuration call, **not** complete control of resource lifetime, engine enablement, and presentation. **Pragmata, Hogwarts Legacy, and Resident Evil Requiem are the three documented FG-continuation successes** in this test set; their different integrations and the other failures do not establish NVIDIA internal mechanisms or an engine-wide compatibility rule.

### Fixed MFG multiplier limits: Dawnwalker and Indiana Jones (October 11, 2026)

On an RTX 5090 with tester-reported driver **610.47**, ReflexProbe's fixed 2X–6X multiplier override was tested against DRM-free GOG builds, leaving each game's selected menu multiplier at 2X. The override operates on game-originated SetOptions and does not require GetState interception.

| Game | Game interposer / NGX FG plugin | Working overrides | Rejected request / fallback |
| --- | --- | --- | --- |
| **Blood of the Dawnwalker** | **2.7.30.0 / 2.14.0.0** | **2X–4X** confirmed, with generated-inclusive output also checked through Steam's overlay. | **6X** returned `38`; **5X** was user-observed disengaging FG but lacked a separate preserved failing setter. Unsupported requests stopped FG; 4X restored it. |
| **Indiana Jones and the Great Circle** | **2.7.32.0 / 2.14.0.0** | **2X–4X**, with GetState reporting a **4X** maximum and presented-frame telemetry reflecting accepted values. | **5X/6X** returned `38`, while FG continued at previously accepted **4X**. |

`38` means **`sl::Result::eErrorInvalidState`**, not a universal mandate to stop generation. Indiana Jones reported `numFramesToGenerateMax=3` (4X). **Dawnwalker's GetState interception was disabled**; it has no measured reported maximum. Equal FG plugin versions cannot explain the different fallbacks by themselves. GetState's transient per-call presentation counts do not define persistent state-change comparisons.

### FG resource retention and its limits

NVIDIA's [Streamline DLSS-G guide, §6.4](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#64-disabling-frame-generation) says explicit Off usually releases internal FG allocations and recommends `eRetainResourcesWhenOff` (`0x08`) for short-term Off transitions, with `slFreeResources()` for long-term disablement. **ReflexProbe does not intercept `slFreeResources()`**; forcing the flag cannot guarantee allocations or generation continue. Menu detection can independently influence retention ([§6.5](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#65-automatically-disabling-dlss-g-in-menus)).

NVIDIA's [§1.1 total FG memory estimates](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#11-run-time-performance) are about **272 MB at 1080p**, **489 MB at 1440p**, and **725 MB at 4K**. Those are integration-dependent *total footprint estimates*, not measured incremental memory from forcing retention. Retention On/Off remains a separate experiment from forcing the FG mode.

## Per-game results

### GSyncProbe (controlled reference)

A controllable D3D11 / D3D12 / Vulkan reference target reproduced ~225 FPS on D3D at 240 Hz with Reflex, VSync, and G-SYNC, versus ~240 FPS on Vulkan without an equivalent automatic cap. It provided direct validation that replacing the explicit Reflex interval with **165, 60, or 30 FPS** changes measured presentation rate, including with Reflex Low Latency Off. Its baseline zero request must not be confused with a game-authored 225 FPS cap.

### Cyberpunk 2077 (GOG)

D3D12, modern Streamline. Repeated Reflex setter calls; all tested modes requested a zero explicit Reflex interval.

**FG Off override:** in commonly used gameplay menus, forwarding On rather than the game's requested Off **did not keep FG running**. That does not rule out other menu or scene-specific behavior.

### The Witcher 3 Remastered

D3D12, modern Streamline. Reflex probing previously observed repeated Off/On/On + Boost settings with zero explicit interval.

**October 8, 2026 — standalone FG probing:** the DRM-free GOG build was successfully acquired with **Launch + Inject** outside Galaxy and **Watch + Inject** through GOG Galaxy, including **FG-only probing** with Reflex disabled. Both `slDLSSGSetOptions` and optional `slDLSSGGetState` were intercepted; the FG plugin reported **2.14.1.0**.

Successful game requests included **2X** (`numFramesToGenerate=1`), **3X** (`=2`), and **6X** (`=5`) with `mode=On`. GetState reported a **6X maximum** (`numFramesToGenerateMax=5`) and Dynamic MFG support, but no captured `mode=Dynamic` requests. These settings/capabilities alone do **not** measure actual native-rendered or generated-frame output.

Entering/leaving menus produced explicit `Off` / `On` SetOptions calls. Fullscreen menu detection remained **unset** (`flags=0x00000000`) in the captures. UI recomposition was Yes when FG-On and No when FG-Off; updated multipliers appeared on returning to gameplay. Tens of thousands of identical SetOptions submissions were compressed as repetitions, separately from GetState and Reflex. Transient GetState presented-frame counts were not treated as persistent state changes.

**FG Off override outcome:** in the commonly used gameplay menus, forwarding On over Off **did not keep FG running**. Not every specialized menu was evaluated. A monitor's ~225 FPS indicator from an earlier test was insufficient to identify actual rendered-versus-generated cadence; further output-rate measurement would require a suitable independent display metric.

### A Plague Tale: Requiem (GOG)

D3D12, legacy Streamline **1.0** plugin gateway. Setter calls roughly follow frame cadence and natively request zero interval in the tested modes. Reflex observation and explicit interval override work.

**Forced Boost performance collapse — PresentMon:** the game natively requested plain **Reflex On**, while ReflexProbe forwarded **On + Boost** with a forced **6329 µs (~158 FPS)** interval. During an approximately **29.6-second** capture, presentation progressively slowed toward ~30 FPS, recovering immediately when **On + Boost was selected natively inside the game's menu**:

| Approximate elapsed time | Present rate | Frame interval |
| --- | ---: | ---: |
| 0–4 s | ~158 FPS | ~6.33 ms |
| 5 s | 101.93 FPS | 9.81 ms |
| 7 s | 63.39 FPS | 15.78 ms |
| 10 s | 45.49 FPS | 21.98 ms |
| 13 s | 37.60 FPS | 26.60 ms |
| 16 s | 32.54 FPS | 30.73 ms |
| 18–26 s | ~30 FPS | ~33.33 ms |
| 27 s | 157.91 FPS | ~6.33 ms |

**GPU busy** measured approximately **2.353 ms at ~158 FPS** versus **2.384 ms at ~30 FPS**, while **GPU wait** increased from approximately **3.975 ms to 30.926 ms**. Nearly identical GPU busy times argue against a conventional increased GPU rendering workload and favor a pacing/waiting problem, but **do not isolate** the application, legacy Streamline integration, or driver as the source.

The immediate recovery when the game itself selected native On + Boost is particularly important: a visibly equivalent forwarded On + Boost mode and interval **did not produce equivalent runtime behavior** under interception. This is an SL1 Force Boost caveat, not a universal result for newer Streamline.

### Blood of the Dawnwalker (GOG)

UE5.5.3 local build, D3D12, modern dynamically resolved Streamline. Frame Generation enabled caused an Off -> On pair with zero intervals; Frame Generation disabled caused one Off call. With G-SYNC + forced VSync, the FG/Reflex-On case exhibited the familiar ~225 FPS at 240 Hz. **Watch + Inject is runtime-confirmed through GOG Galaxy**: the watcher detected the launcher-created executable, injected successfully, and caught the later sparse Reflex startup calls.

**October 11, 2026 — fixed FG multiplier override:** The game submitted `numFramesToGenerate=1` (2X) rather than changing the count itself. With ReflexProbe substituting the count on game-originated `slDLSSGSetOptions` calls, 2X, 3X, and 4X returned `result=0` and were observed working; the tester also confirmed the distinct application/display rates using Steam's performance overlay after adding the DRM-free executable as a non-Steam game. 5X was observed to disengage FG, but the brief preserved log did not separately capture its failing setter. A 6X request (`numFramesToGenerate=5`) explicitly returned **`38` (`sl::Result::eErrorInvalidState`)**, including when the game was forwarding `mode=Off`; FG visibly disengaged. Returning to 4X restored `result=0` and working FG, without a game crash.

The game-local `sl.interposer.dll` reported **2.7.30.0**, while the NVIDIA NGX-loaded **`sl_dlss_g`** (and `sl_reflex`) plugin reported **2.14.0.0**. Tests used an **RTX 5090**, with the tester reporting NVIDIA driver **610.47**. **GetState interception was disabled**, so there is no captured `numFramesToGenerateMax` for Dawnwalker. The observed working ceiling of 4X suggests a title/runtime constraint, but file versions and result code 38 alone do not identify the exact cause. Do not generalize this title's FG-disengagement behavior to every game. **Normal tested Dawnwalker menus already kept FG running**, independently of an Off-to-On override; that is not evidence of a successful override.

### No Man's Sky (GOG)

Vulkan + modern Streamline with an independent Reflex control. ReflexProbe successfully intercepted **10,674** settings calls in one state interval while counting **0 `slReflexSleep` calls**. A forced nonzero `frameLimitUs` was accepted with a successful result but did not impose the requested presentation limit. The game's own limiter is separate and pre-FG. Zero Streamline sleeps is therefore a concrete difference from Indiana Jones, although it does not by itself prove why the limiter is ineffective.

### Indiana Jones and the Great Circle (GOG)

Vulkan + modern Streamline. Reflex/Frame Generation behavior is gated behind DLSS as the selected upscaler: when DLSS is not selected, the Frame Generation option is hidden and the observed Reflex path is effectively absent. With DLSS active, the game repeatedly submits Reflex On with a zero interval at high frequency. In one 158 FPS run ReflexProbe observed **3,856 settings calls and 3,854 `slReflexSleep` calls**, consistent with approximately one setter and one sleep call per rendered frame during the active gameplay interval. Force Boost and frame-limit override both work, while the game's own limiter remains a separate pre-FG limiter.

**October 11, 2026 — fixed FG multiplier override and GetState capture:** In the DRM-free GOG build, the game's selected multiplier was left at **2X** while ReflexProbe substituted 2X, 3X, 4X, 5X, and 6X. The local `sl.interposer.dll` reported **2.7.32.0**; NVIDIA NGX-loaded `sl_dlss_g` and `sl_reflex` plugins reported **2.14.0.0**. With optional `slDLSSGGetState` interception enabled, **every captured GetState call reported `numFramesToGenerateMax=3` (4X maximum)**, `status=OK`, and `result=0`. Raw capture retained **1,904 GetState calls and 1,904 SetOptions calls** with no missing ordinals. GetState's `numFramesActuallyPresented` (shown in ReflexProbe as `framesPresentedSincePreviousGetState`) followed accepted 2X/3X/4X SetOptions overrides with values **2/3/4**, even though the game's own GetState *input options* continued to say `framesToGenerate=1` (the selected 2X). The returned presentation statistic, not that caller-supplied input field, is the relevant evidence of effective output.

When alt-tabbing away, the game's **own top-right performance metrics** could read **FG 1X**; the Raw log also captured intervals with GetState reporting **one presented frame**, meaning no extra generated frames in those sampled intervals. `mode=On` alone therefore does not guarantee that extra frames were presented on every call. Changes in these per-call presentation counts are deliberately excluded from **State changes** comparisons as transient telemetry; the first GetState call is still displayed, and unchanged persistent fields are summarized as repetitions. The **Detailed FG diagnostics** checkbox only changes SetOptions presentation, not GetState filtering.

Forwarding **5X** (`numFramesToGenerate=4`) and **6X** (`=5`) returned **`38` (`sl::Result::eErrorInvalidState`)** on repeated SetOptions calls (198 and 204 captured calls, respectively), while GetState continued to report a **4X maximum** and predominantly **four presented frames**. **Unlike Dawnwalker, Indiana Jones continued running FG at the last accepted 4X setting**: its in-game metric showed FG 4X, with perceived input behavior and GPU power savings consistent with FG remaining active. Those user-observed effects are distinct from a direct independent measurement of every physically displayed frame. The game displaying the changed 3X/4X values despite an unchanged 2X menu is strongly consistent with it deriving its performance readout from runtime presentation statistics, though the UI's exact implementation was not inspected.

**Scope:** `38` identifies invalid Streamline state; it is not a universal instruction to turn FG off. Indiana Jones's explicitly reported maximum of three generated frames explains why a request for four or five is unsupported in this tested integration, while Dawnwalker's precise limit remains unmeasured. Both games used an NGX feature plugin reporting 2.14.0.0; that common version is diagnostic context, not proof that the plugin version by itself imposes this limit.

### God of War (2018)

D3D11 + native NVAPI Reflex, independent of whether DLSS is enabled. Runtime-confirmed Off / On / On + Boost observation, with `NvAPI_D3D_SetSleepMode` called only when the menu setting changes. While Reflex was On, `NvAPI_D3D_Sleep` counts tracked approximately frame cadence; during the captured Reflex-Off interval the sleep count was **0**, yet the forced `minimumIntervalUs` limiter remained effective. This demonstrates that, in this shipped native-NVAPI integration on the tested driver, application-observed per-frame sleep calls are not required for the explicit limiter to remain active while low-latency mode is Off. Force Boost and `minimumIntervalUs` override both work.

### Shadow Warrior 3 (GOG)

native NVAPI D3D Reflex. The outer `SW3.exe` is an Epic `BootstrapPackagedGame` wrapper, so Launch + Inject targets the wrong process and finds no Reflex path. Watch + Inject against the exact full path `SW3\Binaries\Win64\SW3.exe` catches the real game. SetSleepMode is submitted only on Reflex state changes; frame-limit override and Force Boost work. With Reflex Off, the captured Sleep count was **0** while the forced 6329 us (~158 FPS) limiter still worked. With Reflex On / On + Boost, Sleep counts were approximately frame-cadence. No Frame Generation path is present in the tested title.

### Pragmata (Steam)

D3D12 + modern Streamline 2.8.0. Watch + Inject acquired the Steam-launched executable successfully on the tested Denuvo-protected build, the frame-limit override worked, and Reflex settings were submitted only on option changes while `slReflexSleep` continued at approximately rendered-frame cadence. The tested DRM configuration did not block ReflexProbe's injection/interception path; that observation is specific to this build and is not a claim about every Denuvo integration.

**October 12, 2026 — successful forced FG continuation:** after a successful native active SetOptions call established history, ReflexProbe converted later game-requested `mode=Off` into active `On` settings, retaining the last successful native frame count unless a selected fixed multiplier replaced it. Multiple converted calls returned **`result=0`**, including a tested **3X** forwarding configuration. Steam's overlay showed Frame Generation continuing while the game asked for Off; the tester reported **no artifacts**. The combination of captured successful requests and user-observed generated output makes this the strongest continuation result.

Pragmata also submitted an **initial Off call containing a nonzero native frame count** before any qualifying active state. The later **Always** policy permits forcing that first call On immediately rather than waiting for previous successful On/Auto history, and aggressive initial activation was also reported successful. That observation does not imply other games will behave the same way.

### Resident Evil Requiem (Steam; D3D12 / RE Engine)

**October 14, 2026 — successful FG Off-to-On continuation.** In the tested Steam release, **Watch + Inject** acquired `re9.exe` by its exact path and hooked the modern `slGetFeatureFunction` resolver through `re9.exe GetProcAddress IAT`. The game's `sl.interposer.dll`, `sl.reflex.dll`, and `sl.dlss_g.dll` reported **2.8.0.0**, the same reported Streamline version as the tested Pragmata integration. That version match alone does **not** establish identical RE Engine internals or explain their similar behavior.

**Reflex:** The game's initial On + Boost request contained `frameLimitUs=0`. ReflexProbe forwarded **6329 µs (~158.003 FPS)** and the setter returned **`result=0`**; the tester confirmed that the frame-rate override worked. Reflex Sleep interception was enabled. **FG capability:** optional `slDLSSGGetState` interception on viewport **0**, using `stateVersion=3`, reported `status=OK`, `maxFramesToGenerate=3` (**4X maximum**), and successful results. The game's own FG menu likewise exposes multipliers no higher than 4X. The observed MFG maximum is **not** a test of forcing unsupported 5X or 6X.

**FG mode experiment:** Initially the experimental Off override was disabled and game-requested Off was forwarded unchanged. A successful native `mode=On, framesToGenerate=2` request at **12:55:06** established the per-viewport active history. The operator then toggled **Override FG Off / After success** on, off, and on again **without changing the other active overrides**. Key excerpts (all `slDLSSGSetOptions` calls on viewport **0**):

```text
12:55:28  #17418  mode=Off (0),                         result=0
12:55:30  Override FG Off: After success enabled
12:55:30  #17630  mode=Off (0) -> On (1),              result=0
12:55:50  Override FG Off: disabled
12:55:50  #18900  mode=Off (0),                         result=0
12:56:01  Override FG Off: After success enabled
12:56:01  #20253  mode=Off (0) -> On (1),              result=0
```

The independent fixed **3X** multiplier, **Force Auto to On**, **fullscreen-menu detection forced Off**, and **resource retention forced On** remained enabled; the fixed Reflex limit and Force Boost were also active. This comparison specifically demonstrates that the **Off checkbox changed the forwarded mode while the other selected policies remained constant**, rather than attributing continued generation to an untested single setting. All quoted setters returned success, but the tester's separate observations are what establish the **realized FG-continuation outcome**.

**Observed result:** FG continued successfully when forcing On in the main menu, map, an inventory menu occupying roughly 30% of the screen, and multiple additional menus **including video playback**, with behavior comparable to Pragmata. This is a **successful FG Off override across the tested menu transitions**, not a claim that every possible screen or game build was exercised. Unlike the negative results in Cyberpunk 2077 and The Witcher 3 Remastered, ordinary game-requested menu Off calls could be overridden here while maintaining FG. This run establishes success for **After success**; it does **not** separately validate the aggressive **Always** startup mode.

**Additional diagnostics:** The first captured FG setter was game `mode=Off, framesToGenerate=1` (**2X**) while the fixed multiplier forwarded **2 generated frames (3X)**; mode remained Off because the Off override was not yet enabled. At **12:56:45**, another game `Off` call with native **2X** was forwarded as **On, 3X**, with the menu flag already Off and the retention flag changed from Off to On; it returned `result=0`. The session ended cleanly (`exit code 0`). The final diagnostics recorded **22,675 FG SetOptions calls**, **22,673 Reflex Sleep calls** since the first Reflex state, and a large GetState repetition run (**22,670 repeats since call #5**). Those high call counts are diagnostic context, **not** independent measurements of actual generated frames or their presentation rate. Transient GetState presentation counts were suppressed in State changes mode and must not be interpreted as a continuous FG presentation measurement.

### Deadzone: Rogue

**FG Off override failure despite `result=0`.** Intercepted game-requested `Off` was forwarded as `On` and the API accepted the call, but generation **stopped**. GetState's presented-frame statistic dropped from **three** during active FG to **one** after the disabling transition; other tester-observed performance/display behavior agreed. Accepted SetOptions does not guarantee a corresponding generated output. The exact engine or resource-lifecycle cause was not established.

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

Captured Reflex-Off intervals produced **0 sleeps**. The active rates are consistent with the existing Dawnwalker / DOOM hypothesis that Reflex Sleep follows rendered-frame rather than FG-inclusive cadence under 2X FG, but this particular older Reflex-only sample remains inferential even though newer ReflexProbe builds now support direct FG telemetry.

**Later FG experiments:** Satisfactory's fixed multiplier override demonstrated **3X and 4X** working despite an exposed default 2X setting. Similar multipliers may also be selectable through Unreal Engine configuration files; the API intervention remains useful diagnostically. In contrast, forcing a game-requested **FG Off** to **On** **did not keep generation running**, even though the forwarded request was accepted. The game also changed Reflex mode during the transition, complicating attribution. An NVIDIA driver-profile override initially interfered with multiplier testing and was reset before the successful 3X/4X results were attributed to the forwarded settings.

### The Talos Principle 2 (Steam; UE5 / modern Streamline)

**October 12, 2026 — Reflex frame-limit override works.** The tested UE5 build accepted ReflexProbe's `frameLimitUs=6329` override (approximately **158.003 FPS**), and the tester confirmed that the **explicit Reflex frame limit actually governs the frame rate**. This is the original primary purpose of ReflexProbe, distinct from the later Frame Generation experiments. The captured Reflex transition from the game's `mode=Off, frameLimitUs=0` to `mode=On, frameLimitUs=0` was forwarded with the requested 6329 µs interval and, for On, Force Boost; both setter calls returned `result=0`.

**Frame Generation remains enabled in menus by the game's own policy.** The Talos Principle 2 did not automatically disable FG in menus when first tested and still does not in the updated build. The **experimental FG Off-to-On override was disabled** for this session: continued FG in menus is not an achievement of that override. The log contains a game-requested Off call later in the session, but it does not establish a menu-triggered disable; the tester's observation concerns ordinary menu navigation. This is a useful UE5 counterexample to games that deliberately turn FG Off at menu transitions.

The game originally shipped on **Unreal Engine 5**; its earlier Multi Frame Generation support reached **4X**, and a later update added **5X and 6X**. The tested build's local `sl.interposer.dll`, `sl.reflex.dll`, and `sl.dlss_g.dll` report **2.11.1.0**. `slDLSSGGetState` reported `stateVersion=4`, `maxFramesToGenerate=5` (**6X maximum**), and `dynamicMFGSupported=Yes`. The game explicitly submitted `framesToGenerate=5` (6X); while ReflexProbe's fixed 3X multiplier override was active, this was forwarded as 3X. After the multiplier override was disabled, the game's own `mode=On, framesToGenerate=5` call was forwarded unchanged and returned `result=0`. The reported 6X capability and successful setter are not, by themselves, a measurement of every physically presented generated frame.

Watch + Inject acquired `Talos2-Win64-Shipping.exe` via the game's main `GetProcAddress` IAT. SetOptions used viewport **1**, while the intercepted GetState calls reported viewport **0**; that distinction prevents equating the displayed GetState transient presentation count with direct confirmation of the configured viewport's output.

### Hogwarts Legacy (Steam; D3D12 / modern Streamline)

Hogwarts Legacy is a useful historical bridge specimen: it shipped as a **heavily customized late Unreal Engine 4** title in February 2023 with the original RTX 40-series style of ordinary 2X Frame Generation, then later received modern Multi Frame Generation support after the RTX 50-series launch. The current tested build supports 2X / 3X / 4X Frame Generation once FG has been enabled; turning the user-facing FG setting from Off to On or fully back Off requires a restart, while switching between active multipliers does not. This settings-menu restart requirement is separate from the temporary FG Off requests the game issues around menus.

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

Changing the exposed 2X / 3X / 4X Frame Generation multiplier did not itself produce a Reflex state change. This is consistent with FG policy changing independently while Reflex remains enabled.

**FG Off override success:** in the user's testing, overriding the game's temporary **FG Off** requests to **On** successfully kept DLSS Frame Generation operating throughout **all tested menus**, just as observed in Pragmata. Hogwarts is therefore a second concrete positive example of choosing to keep FG active during ordinary menu navigation. The game's **heavily customized UE4 integration** is substantially unlike a stock UE4 implementation, so this result must **not** be generalized to UE4 titles as a group. This records the observed continued FG operation; it does not imply that every possible scene, every build, or the aggressive **Always** policy was separately validated.

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

Intercepted calls are forwarded and examined, but identical settings calls are not individually retained in State changes mode. Reflex, FG SetOptions, and FG GetState have separate state runs and repetition counters; FG is keyed by viewport.

- Reflex transitions use **`NEW Reflex State:`**; FG distinguishes **`NEW FG SetOptions:`** from **`NEW FG GetState:`**;
- repeated calls and changes confined to fields suppressed by the compact FG view do not each print a new visible state;
- repetition summaries refer to the preceding **numbered API call** and (for FG) **viewport**; optional Reflex Sleep summaries refer to their preceding Reflex settings call;
- no zero-repetition line is printed, and pending counts are flushed on state/display/capture changes or target exit;
- compact FG shows original **requested → forwarded** fields (unchanged values printed once); Detailed FG diagnostics exposes the complete SetOptions fields and Raw debug is fully detailed.

This makes the normal gameplay footprint essentially independent of whether an engine calls the Reflex setter twice per launch or hundreds of times per second for hours.

#### Raw debug

**Raw debug** is an explicit debugging/tuning mode.

Selecting it starts a new raw capture from that point forward. ReflexProbe does not reconstruct or pretend that calls from before Raw debug was selected were captured. Every subsequently drained Reflex setter, FG SetOptions, or FG GetState event is displayed individually and retained in a fixed **131,072-event RAM ring**. When **Count Reflex Sleep Calls** is enabled, each wrapped sleep call is also displayed and retained individually. Settings and sleep calls keep separate call numbers even though they share the same transport ring. Once the ring is full, the oldest retained raw calls are overwritten by new ones.

The ring is allocated only when Raw debug is selected; retained memory use is bounded. Live raw text is still batched at the controller's 50 ms poll cadence, so the Win32 EDIT control receives at most about 20 batch appends per second rather than one mutation per Reflex call.

Switching from State changes to Raw debug first closes the current compressed state run. Switching back stops raw retention and starts a fresh state-change run with the next Reflex call. The most recent raw ring remains in RAM until it is cleared, replaced by a new Raw debug session, or the controller exits.

The **Clear** button empties the visible status log, frees any retained Raw debug ring, and reprints startup diagnostics so a new diagnostic pass can begin without restarting ReflexProbe. It is intentionally available only while idle: it is disabled while a target is injected and while Watch is armed, so Clear never races the injected DLL's 4096-event shared transport ring or discards a live state/sleep-count run.

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

ReflexProbe's direct `slDLSSGSetOptions` and optional `slDLSSGGetState` interception is **implemented**, not future work. Outstanding questions concern why **Pragmata, Hogwarts Legacy, and Resident Evil Requiem** maintain FG after forced active requests while **Deadzone: Rogue, Satisfactory, Cyberpunk 2077, and The Witcher 3 Remastered** do not in the tested scenarios. Independent Reflex state changes, game-managed resource lifetime, and actual presentation remain difficult to isolate from an accepted configuration call alone. Overriding retention does not block `slFreeResources()`.

Additional tests could investigate 5X/6X support limits, NVIDIA driver-profile interference, and genuine capabilities of legacy Streamline implementations. Native Vulkan `VK_NV_low_latency2` interception and SL1 Sleep counting are not currently implemented.

The test set does not establish the underlying driver pacing mechanism for the Reflex-Off / zero-Sleep / working-limit cases, nor does it identify why No Man's Sky accepted a nonzero interval without following that limit. Those remain open observations rather than resolved explanations.
