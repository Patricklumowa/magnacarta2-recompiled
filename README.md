# Magna Carta 2 Recompiled

**An experimental PC port of the Xbox 360 version of Magna Carta 2, built with static recompilation.**

Magna Carta 2 Recompiled (MC2Recomp) uses [ReXGlue](https://github.com/rexglue/rexglue-sdk) to translate the game's PowerPC executable into C++ and compile it for Windows. Game-specific hooks provide controller fixes, frame pacing, and PC settings. Graphics currently use ReXGlue's Xenos compatibility renderer; replacing the CPU execution path does not remove the cost of translating the original GPU workload.

> [!IMPORTANT]
> This is an early development project. Boot, movies, the title menu, and initial gameplay have been reached, but audio and performance remain unfinished. There are no packaged releases yet.
>
> You must supply game files extracted from your own copy. This repository contains project source, configuration, and SDK patches. It does not include game assets, executables, disc images, or generated game code.

## Current status

| Area | Status |
| --- | --- |
| Boot and movies | Startup logos, intro movie, and title screen reached |
| Gameplay | Initial gameplay and movement tested; a complete playthrough is unverified |
| Controllers | Input and disconnect/reconnect tested; device compatibility is not exhaustive |
| PC settings | F4 menu with 720p, 1440p, and 4K rendering presets |
| Frame rate | 30 FPS, 60 FPS, and experimental unlocked modes; targets are not performance guarantees |
| Graphics | D3D12 compatibility renderer; original shadow/tiling behavior retained |
| Performance | Substantial overhead and stutter remain; optimization is in progress |
| Audio | Not working in current gameplay tests; enabling output does not resolve missing audio support |
| Saves and progression | Full save/load, combat, and chapter coverage remain unverified |
| Multiple discs | Content catalog tooling exists; automatic disc switching is not implemented |
| Platforms | Windows x64 is the tested build target; other platforms are unvalidated |

## Building

> The build and launch instructions below describe the local development setup. Python helpers under `tools/` are maintained locally and are not distributed in this repository. A fresh clone does not include all helpers required by this workflow.

### Requirements

- Windows x64 and a Direct3D 12 capable GPU. The current ROV rendering path also needs rasterizer ordered view support.
- An x86-64 CPU supporting the x86-64-v2 instruction set, including SSSE3 and SSE4.1.
- [Git](https://git-scm.com/downloads) and Python 3.11 or newer on `PATH`.
- Visual Studio 2022 or its Build Tools, with **Desktop development with C++**, **C++ Clang tools for Windows** (Clang 18 or newer), **C++ CMake tools for Windows** (CMake 3.25 or newer and Ninja), and a Windows SDK. The build helper locates these through `vswhere`.
- The Windows AMD64 [ReXGlue SDK v0.10.1](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.1) package. This project is pinned to that version.
- Locally extracted game data. Allow additional disk space and RAM for generated sources, the SDK, and compiler output. Reduce `--jobs` if compilation exhausts memory.

### 1. Clone the project

```powershell
git clone https://github.com/Patricklumowa/magnacarta2-recompiled.git
cd magnacarta2-recompiled
```

### 2. Add your game files

Keep the original directory structure beneath each disc folder:

```text
magnacarta2-recompiled/
  Magna Carta 2 Disc 1/
    default.xex
    ...original disc contents...
  Magna Carta 2 Disc 2/
    default.xex
    ...original disc contents...
  native/
```

Disc 1 supplies the executable and current runtime content. Both discs are required for the two-disc audit and content catalog. Keep their contents separate: copying one over the other can overwrite files with conflicting paths.

The supported executable has title ID **`4E4D080C`** and the following SHA-256 for its **decoded, page-padded executable image** (this is not the hash of `default.xex`):

```text
f22261518998a368511f33d533dd8d07e1844efcb7c618e49817dc5c5d936548
```

Both tested discs decode to this same image. Other regions, revisions, title updates, and modified executables are unverified; the hooks and function mappings target this exact version.

To verify both extracted executables:

```powershell
python -m pip install -r requirements-dev.txt
python tools/check_mc2_audit.py
python tools/check_mc2_analysis.py
```

These checks read your game files locally. They do not upload or modify them.

### 3. Install the pinned SDK

Download `rexglue-sdk-0.10.1-win-amd64.zip` from the linked ReXGlue release and extract it so this file exists:

```text
tools/rexglue-sdk-0.10.1/win-amd64/bin/rexglue.exe
```

The release archive's SHA-256 is:

```text
26dbcd19e6976a7ad349917bffc58c1a0f21ad43d71a963d16b623c8e70d1f4d
```

The SDK installation stays local and is ignored by Git.

### 4. Compile

Run from the repository root:

```powershell
python tools/build_native.py --build-gpu --jobs 4
```

This clones ReXGlue v0.10.1 into `thirdparty/rexglue-sdk`, verifies revision `bd833a2ab69d305f99ddf57418fc38ffec8af93d`, initializes its required submodules, applies the patches in `native/patches`, and builds the runtime and GPU plugin. It then generates the game sources locally, checks their control flow, builds the application, and runs the native regression checks.

The executable and its runtime files are staged in:

```text
native/out/build/windows/magnacarta2.exe
```

Use `--build-gpu` for subsequent builds too: it stages the patched GPU plugin with the application. A build without this flag may stage the official SDK GPU plugin instead. Existing build outputs are reused where possible.

For code generation and CMake configuration only:

```powershell
python tools/build_native.py --configure-only
```

Generated game code remains under `native/generated/default/` and is excluded from Git. The tracked `native/generated/rexglue.cmake` file is SDK build integration, not translated game code.

## Running

Use the development launcher with a persistent profile:

```powershell
python tools/smoke_boot.py --until-exit --profile out/user --render-target-path rov
```

The launcher writes logs beneath `out/boot-<timestamp>/`, shares the cache under `out/cache/`, and uses `out/user/` for profile data. Reuse the same `--profile` path to keep that data between runs. Without `--profile`, each launch creates a separate test profile. Back up profile data before experimenting with runtime changes.

`--until-exit` keeps the launcher attached until you close the game. Without it, this tool is a timed smoke test that closes the game after 30 seconds by default.

### Controls and settings

- Use a controller for gameplay; XInput/SDL input is provided by ReXGlue. Press the controller's **Start** button at the title screen.
- Press **F4** for PC settings. Choose 720p, 1440p, or 4K rendering and 30 FPS, 60 FPS, or **Unlocked (experimental)**. Save settings and restart for resolution and frame-rate changes to apply.
- `--framerate 30`, `--framerate 60`, or `--framerate unlocked` overrides the frame-rate mode for a launch.
- The development launcher mutes audio by default. `--sound` enables output for audio debugging, but functioning game audio is still unfinished.
- `--adapter N` selects a DXGI adapter when needed; adapter numbers depend on your system.

## Known limitations and development goals

The renderer still translates Xbox 360 draw commands, shaders, and render-target behavior. High host presentation counts do not necessarily mean equally high game-frame delivery. Use `--frame-stats` to log guest frame intervals when investigating pacing; report the scene, resolution, frame-rate mode, and hardware with any performance comparison.

The current priorities are consistent frame delivery while preserving the original visuals, working audio, broader gameplay/save testing, and automatic access to both discs' content. Unlocked timing needs validation across gameplay and cutscenes. No completion date or whole-game performance target is promised.

Experimental shader output/sample specialization and the fast constant-copy hook remain opt-in. Their presence in the source does not establish a validated whole-game improvement. GPU timing capture is also off by default.

Native graphics migration has started with opt-in original shader capture, a
verified partial graphics map, and raw guest-submission timing. These are
diagnostics; the game still renders through Xenos. See the
[implementation status and capture workflow](native/docs/native-renderer.md).

## Development

| Path | Purpose |
| --- | --- |
| `native/src/` | Application integration, PC settings, and game hooks |
| `native/magnacarta2_manifest.toml` | Pinned SDK version, game path, and recompilation entry point |
| `native/config/` | Function discovery and boundary hints |
| `native/patches/` | Controller, frame statistics, and renderer changes against the pinned SDK |
| `native/tests/` | Native regression checks |

Additional checks:

```powershell
python tools/check_content_catalog.py
python tools/check_codegen.py
```

The catalog check uses synthetic files and requires no game data. The code-generation check requires a prior codegen/build. The executable audit and analysis checks shown above require your extracted discs. `build_native.py --build-gpu` runs the native checks for rounding, concurrent controller access, frame pacing, constant copying, register uploads, and sample addressing.

`tools/catalog_content.py` hashes both disc trees and records shared files and conflicts under `out/`; it does not merge content or implement disc switching. `tools/analyze_mc2.py` writes executable metadata locally and optionally produces an entry disassembly when `llvm-mc` is on `PATH`.

### Contributing

Keep changes focused and describe the behavior, relevant scene, and validation. Performance changes should compare the same view and settings with the original visual effects intact. Distinguish automated test coverage from actual gameplay testing.

Use conventional commit prefixes, for example `fix(input): ...`, `perf(renderer): ...`, `feat(settings): ...`, `build: ...`, `test: ...`, and `docs: ...`.

Commit source, build configuration, patches, and documentation only. Do not commit extracted game files, recompiled game code, SDK checkouts, binaries, caches, saves, logs, or captures. `.gitignore` uses an allowlist; update it deliberately when adding a new source directory. Do not attach game data to issues or pull requests.

## Credits and notices

- [Softmax and Namco Bandai Games](https://www.unrealengine.com/blog/magnacarta-2?lang=en-US) developed and published the original Magna Carta 2, built with Epic Games' Unreal Engine 3.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) provides the recompilation toolchain and runtime used by this project.
- [Xenia](https://github.com/xenia-project/xenia) provides foundational Xbox 360 research and technology incorporated into ReXGlue.
- [SIMDe](https://github.com/simd-everywhere/simde) provides portable SIMD support used by the toolchain.
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) is a reference for the wider Xbox 360 recompilation effort and project documentation.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) supplies the shader-conversion tool being evaluated for native graphics; MC2 shader compatibility is not yet established.
- [plume](https://github.com/renderbag/plume) is the cross-platform rendering library evaluated for the planned native bridge.

Third-party source and patch context retain their upstream terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). A project-wide license for original contributions has not yet been selected.

Magna Carta 2 and its game content belong to their respective rights holders. This is an unofficial project, not affiliated with or endorsed by the game's developers, publishers, or Microsoft.
