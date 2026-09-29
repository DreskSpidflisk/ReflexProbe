# ReflexProbe

ReflexProbe is a deliberately small Win32 tool for observing and overriding the frame-limit value applications send to NVIDIA Reflex.

The first bootstrap build focuses on **Streamline Reflex** and **directly launched x64 games**.  It does not render an overlay, modify shaders, replace Streamline DLLs, or know anything about Steam/GOG/Ubisoft launchers.

## Why this exists

GSyncProbe reproduced a repeatable API split with Reflex enabled and no explicit Reflex frame limit:

- D3D11 @ 240 Hz: about 225 FPS
- D3D12 @ 240 Hz: about 225 FPS
- Vulkan @ 240 Hz: 240 FPS

UE 5.8.3 source also showed that its native Reflex path normally passes a zero minimum interval unless the engine has an explicit max tick rate.  ReflexProbe is intended to answer the next question directly: **what did the game actually ask Reflex for?**

The useful field is `sl::ReflexOptions::frameLimitUs`:

- `0` = no explicit Reflex frame limit / NVIDIA automatic behavior
- nonzero = explicit minimum frame interval in microseconds

When override mode is enabled, ReflexProbe preserves the game's Reflex mode and every other option, copies the options structure, changes only `frameLimitUs`, and forwards the copy to the real Streamline Reflex function.

## Solution layout

```text
ReflexProbe.sln
    ReflexProbe      - Win32 controller / direct-launch injector
    ReflexProbe64    - x64 injected DLL / Streamline Reflex hook

Common/Protocol.h    - tiny shared-memory protocol
ThirdParty/MinHook   - MinHook v1.3.4 git submodule, compiled statically into ReflexProbe64
```

Both projects write to:

```text
bin\x64\Debug\
bin\x64\Release\
```

There is no MinHook runtime DLL.  Its source is compiled directly into `ReflexProbe64.dll`.

## Build prerequisites

The project intentionally follows the same Visual Studio conventions as GSyncProbe:

- Visual Studio 2022 / v143
- Windows 10 SDK
- x64 only for now
- `STREAMLINE_SDK` environment variable pointing at an NVIDIA Streamline SDK root containing `include\sl_reflex.h`

After pulling the repository for the first time, initialize MinHook:

```bat
git submodule update --init --recursive ThirdParty/MinHook
```

Then open `ReflexProbe.sln` and build Debug or Release x64.

## Bootstrap workflow

1. Start `ReflexProbe.exe`.
2. Browse to a DRM-free x64 game executable.
3. Leave **Override Reflex frame limit** unchecked for the first observation run.
4. Choose **Launch + Inject**.
5. ReflexProbe creates the game, injects `ReflexProbe64.dll`, and resumes it.
6. The injected DLL waits for the actual loaded `sl.reflex.dll`, reports its full path and file version, resolves `slReflexSetOptions` through the plugin's `slGetPluginFunction`, and hooks that implementation with MinHook.
7. Every observed call reports the Reflex mode, requested `frameLimitUs`, effective value, and Streamline result.

For an override test, enable the checkbox and enter an FPS target before launch.  The controller converts FPS to microseconds using:

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

Changing the override while the game is running updates shared configuration immediately, but the bootstrap build applies it on the **next game call to `slReflexSetOptions`**.  A later version can add an explicit safe reapply path.

## Good first test targets

- Cyberpunk 2077 (GOG): D3D12 + Streamline
- No Man's Sky (DRM-free): Vulkan + Streamline
- Pragmata and other launcher titles: later, after runtime attach/watch is added

Native NVAPI Reflex titles such as God of War (2018) and A Plague Tale: Requiem are intentionally a later backend.  Native Vulkan `VK_NV_low_latency2` interception is also later; Streamline comes first because it covers the modern D3D12/FG ecosystem and gives us the cleanest initial experiment.

## Deliberate limitations

The bootstrap build is not a general-purpose injector yet.

- direct launch only
- x64 only
- Streamline `sl.reflex.dll` only
- no runtime attach/watch yet
- no NVAPI hook yet
- no native Vulkan hook yet
- no automatic reapply when the UI value changes unless the game calls `slReflexSetOptions` again
- no anti-cheat support

Do not use ReflexProbe with anti-cheat/protected multiplayer titles.  The intended test set is DRM-free/offline/no-anti-cheat software where process injection is not fighting a protection system.

## Planned next steps

1. Prove observation on a D3D12 Streamline game.
2. Prove `frameLimitUs` override changes actual Reflex pacing.
3. Repeat on No Man's Sky / Vulkan Streamline.
4. Add runtime **Attach** and **Watch for process** modes so normal Steam-launched games are usable without ReflexProbe knowing anything about Steam.
5. Add native NVAPI Reflex interception (`NvAPI_D3D_SetSleepMode`).
6. Add native Vulkan Reflex interception (`vkSetLatencySleepModeNV`) when a game requires it.

The architecture is intentionally boring: Win32 GUI outside the game, one injected DLL inside it, and one integer under the microscope.
