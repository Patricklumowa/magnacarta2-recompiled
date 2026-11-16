# Native graphics migration

The target is an MC2-specific graphics bridge above Xbox GPU command packets,
retaining ReXGlue's recompiled gameplay and system services. Windows D3D12 comes
first; Vulkan and Linux follow after visual and timing validation. The working
Xenos backend remains the reference. Shadows and other effects are required.

## Current implementation

- Opt-in capture at vertex/pixel shader creation and binding. Binding capture
  reconstructs split virtual/physical data and records that provenance. The
  hooks still execute the original functions; no shader or draw is replaced.
  See [graphics-map.md](graphics-map.md).
- An opt-in runtime patch records individual `VdSwap` submission intervals.
  These measure guest submission cadence, not GPU completion, display scanout,
  or distinct simulation updates. The previous two-second summary logs cannot
  establish per-frame percentiles.
- Local analysis inventories shaders, fingerprints reference builds, computes
  frame-time percentiles, and compares repeated runs. Python helpers are local
  workspace tools, excluded from the public repository under its current policy.
- A pinned XenosRecomp patch fixes sampler-array declarations and masked fetch
  output. All 187 captured boot/gameplay shaders now compile to DXIL, versus
  172 with upstream. This is compilation coverage of the captured set only;
  most native input mappings, mini-fetch cases, and rendered results remain unverified.

An optional [standalone D3D12 probe](native-draw-probe.md) now renders a captured
movie draw using its translated shaders, three video planes, vertices, and pixel
constants. Its 1280×720 output agrees with an independent CPU calculation to
within one UNORM channel step. This is an offline single-draw check; there is
**no in-game native rendering backend yet**. Captures are not a speedup, and
complete-frame rendering compatibility remains unverified.

The optional bounded live movie comparison uses the same native drawing code
on live guest inputs. Two movie draws passed the CPU comparison during a clean
boot; the original renderer remains enabled and still owns game presentation.
The [bootstrap and completion map](native-bootstrap.md) records the device
initialization, marker and resource-retirement dependencies for the next boundary.

## Capture workflow

Build the game with `src/shader_capture.cpp`, and apply the frame-capture SDK
patch after the frame-stats patch. Build the runtime and run the CMake targets
`mc2_shader_capture_check`, `mc2_shader_capture_hook_check`, and
`mc2_frame_csv_check`. The latter requires the
patched SDK source checkout in the workspace.

Set these options at launch, with local absolute paths:

```text
--mc2_shader_capture_dir=<new capture directory>
--guest_frame_csv=<new CSV filename>
```

The CSV parent directory must exist; existing files are refused. Capture is
limited to 36,000 intervals and flushed every 120 intervals and on clean exit.
A forced termination may lose the buffered tail. Shader capture creates a fresh
session subdirectory, preserves original bytes, deduplicates with byte comparison,
and writes creation call sites to `index.csv`. Limits are 16 MiB per container,
256 MiB per session, 4,096 distinct files, and 100,000 creation events. Invalid
memory ranges are skipped; filesystem failures disable capture without replacing
the original game call. Capture memory validation currently supports Windows.

Both options default off. Shader capture does disk I/O and must be disabled for
performance comparisons. Keep game-derived containers, CSVs, and captures in
ignored output directories; never commit them. Treat the container inputs as
untrusted when passing them to development shader converters.

## Verified checkpoint

The diagnostic build reached gameplay with a copied profile and exited cleanly.
Creation-only capture found seven boot shaders; binding capture recovered the
gameplay set. A subsequent boot verified the Windows high-address shader alias
fix without capture warnings. The final capture-disabled boot also exited cleanly.
The original profile and reference game executable were not replaced.

Native checks cover 56 creation/binding passthrough cases, including a real high
guest alias mapping, malformed ranges, deduplication, original return values,
and unchanged guest context/memory. Raw frame-interval and existing controller,
frame-pacing, and graphics-copy checks pass. Translator fixtures and forward/
reverse patch checks pass. These are not whole-game or native-renderer tests.

## Remaining gates, in order

1. Finish the graphics map: resources, uploads, draws, inline state writes,
   resolves, queries, and completion behavior. Validate the executable hash.
2. Translate captured MC2 shader containers, including observed mini vertex
   fetch and dynamic constant behavior. Preserve unsupported cases as explicit
   failures. See [shader-feasibility.md](shader-feasibility.md).
3. Add `mc2_renderer=xenos|native` with Xenos as default. Inject a native
   `IGraphicsSystem`, adapt detached overlays, and implement verified hooks,
   state snapshots, resource lifetimes, real completion fences, and presentation.
4. Render the courtyard with shadows, transparency, HUD, and effects intact.
   Compare matched captures and three warmed 60-second runs. Keep host presents,
   guest submissions, CPU time, and GPU time separate. Target at least 20% lower
   median frame time without tail regression outside measured variation; the
   broader gameplay target remains sustained 60 FPS at 1440p.
5. Expand gameplay coverage and pipeline prewarming, validate unlocked timing,
   then port through plume to Vulkan and test on actual Linux.

Do not wire a blank renderer or expose an unimplemented native option merely to
make the architecture look complete. Keep missing gates visible and the working
renderer usable. Audio and disc routing are separate work.
