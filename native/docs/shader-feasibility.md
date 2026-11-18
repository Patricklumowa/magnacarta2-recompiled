# MC2 shader feasibility checkpoint

The native shader gate is **partially demonstrated, not passed**. The current
187 captured boot/gameplay shaders all translate to HLSL and compile/link to DXIL
after focused translator fixes. Compilation does not establish native visual
correctness, mini-fetch semantics, or complete game coverage.

## Reproducible probe

```powershell
python tools/check_mc2_shaders.py
python tools/probe_mc2_shaders.py out/gpu-timing-shaders --output out/native-renderer/shader-corpus.json
python tools/probe_mc2_shaders.py out/gpu-timing-shaders --containers "Magna Carta 2 Disc 1/MC2Game/CookedXenon" --output out/native-renderer/shader-corpus-disc1.json
```

The probe deduplicates by stage and SHA-256 of canonical big-endian microcode.
ReXGlue `Shader::Shader` stores host-endian words and `Shader::DumpUcode` writes
those words directly. Windows x64 dumps therefore need per-word endian conversion
before matching the bytecode in an Xbox container. `--dump-endian big` supports
dumps produced by a big-endian host; it is not the correct setting for this PC.

Features come from companion ReXGlue disassembly text, not a complete instruction
decoder. A missing feature in this report does not establish its absence from
the game. Files with no disassembly remain explicitly marked as unobserved.

The optional scanner validates the extents it reads and recognizes the container
signature used by upstream. It does not decompress packages, certify every nested
metadata pointer, or make a candidate safe for the upstream translator. It never
extracts or commits bytecode. Reports and game inputs remain local and ignored.
The commands above use local developer tools; the repository's current source-only
policy excludes `tools/`, so they are not clean-checkout build instructions.

## Observed local corpus (2026-11-16)

The existing `out/gpu-timing-shaders` captures contain 289 unique shaders:
219 pixel and 70 vertex shaders, all with companion disassembly. Among them:

| Observation | Shaders |
| --- | ---: |
| Full vertex fetch | 69 |
| Mini vertex fetch | 62 |
| Dynamic float constant indexing | 18 |
| Texture fetch | 213 |
| Cube instruction | 4 |
| Explicit LOD pattern | 1 |

These captures are earlier gameplay evidence, not a fresh complete playthrough
or proof of coverage for any specific scene. Do not infer material roles such as
skin, shadow, or UI from shader hashes alone; correlate them with draw captures.

A scan of 2,476 files (3,328,049,152 bytes) in Disc 1's CookedXenon directory
found zero structurally accepted containers and 208 rejected signature matches.
This is an uncompressed-byte scan; **it does not prove the original game stripped
reflection**, since package compression or a different container layout can hide
it. It also does not cover Disc 2 or every other disc directory.

## Fresh shader-creation captures

The opt-in capture hooks at `sub_823724C8` (vertex) and `sub_82372120` (pixel)
recorded seven containers during `out/boot-20261116T074415Z`. All seven have
constant-table reflection headers; three vertex shaders legitimately declare
zero constants. Two captured microcodes match the earlier corpus after endian
normalization. This establishes that the original metadata is available at
creation for these shaders, even though raw GPU dumps omit it.

All seven generated HLSL successfully using the unmodified pinned XenosRecomp:

| Stage | Containers | DXIL verification |
| --- | ---: | --- |
| Vertex | 3 | All compile as `vs_6_0` |
| Pixel | 4 | All compile as `lib_6_3`, then link as `ps_6_0` with specialization flags 0 and 2 |

Flags 0/2 exercise alpha-test-disabled/enabled variants; they do not disable
effects in the running game. This is offline compilation only. The pixel shaders'
initial direct `ps_6_0` invocation reported an unresolved `g_SpecConstants` function.
That was an invocation mismatch: upstream deliberately leaves specialization in
a separate library. Compiling the shader as `lib_6_3` and linking an exported
`g_SpecConstants()` implementation resolves it correctly. No translator source
patch, assertion removal, or placeholder implementation was needed.

Local evidence: `out/native-renderer/shader-corpus-live.json` and
`out/native-renderer/shader-proof/results.json`; generated HLSL/DXIL stays beside
the latter report. Each converter/compiler process had a 30-second timeout.
The generated shaders have **not** been submitted to native game draw calls or
compared with reference images. Courtyard, skinning, shadow, and other gameplay
shader coverage remains unproven.

## Concrete translator work

Reference revision: XenosRecomp
`990d03b28a27b50277ee5d8d942e1c5f873869d1` (MIT).

### Gameplay corpus and fixes

Binding hooks expanded the local corpus to 187 intact containers (54 vertex,
133 pixel) during `out/boot-20261116T075257Z`. Every accepted container has a
reflection table header. The capture is incomplete: inaccessible resource
ranges are skipped with diagnostics, so this count is not full game coverage.

With assertions enabled, upstream compiled 172/187. The remaining failures were:

| Failure | Shaders | Correction |
| --- | ---: | --- |
| Undeclared sampler slots inside reflected arrays | 10 pixel | Expand each reflection entry's complete register range and emit each descriptor at its actual slot |
| Empty assignments such as `r5. = iPosition0.;` | 5 vertex | Omit the source-value assignment when no destination component selects a fetched value; retain zero/one writes and predication |

`native/patches/xenosrecomp-990d03b-mc2-shaders.patch` contains these changes and
synthetic C++ checks. Sampler-array elements get distinct names and retain their
original register indices in both DXIL and Vulkan declarations. Single-sampler
names remain unchanged. Unsupported sampler ranges are rejected with a diagnostic
explaining the current 16-slot shared-constant layout; they cannot overlap the
next descriptor block silently. The fetch correction covers vertex and texture fetches;
it does not pretend to implement inherited mini-fetch state.

After the patch, **187/187 compile to DXIL**, with no observed assertion failures
or timeouts. The offline runner uses isolated subprocesses, Windows fault-dialog
suppression, and a 10-second timeout per invocation. Shaders requiring
specialization are compiled as libraries and linked for flags 0 through 3.
These are compilation cases, not changes to live game settings.

Of the captured shaders, 45 match earlier disassemblies containing mini fetch.
All 45 compile. Mini-fetch behavior and input-semantic mapping are now verified
for the first depth pass and the geometry of the first color pass, but remain
unverified across the rest of the corpus. The translator maps fetch instruction
addresses to declaration metadata rather than explicitly modeling inherited fetch state; a successful
DXC compile cannot validate that mapping.

Local reports retain both sides of the comparison:
`out/native-renderer/shader-proof-gameplay/results.json` and
`out/native-renderer/shader-proof-gameplay-patched/results.json`.
Each result explicitly marks visual parity and mini-fetch semantics as unverified.

The patch applies cleanly to the pinned upstream sources; forward application
reproduced all modified files, and reverse application checks passed. Configure
upstream with `XENOS_RECOMP_BUILD_CHECKS=ON` to build `XenosRecompCheck` alongside
the converter. Its synthetic fixtures cover multi-register sampler arrays,
single samplers, masked fetches, constant zero/one writes, mixed destination
swizzles, and predication. Keep converter assertions enabled during feasibility
work; the checked local build uses `/O2 /Ob2` without `NDEBUG`. Boundary checks
accept register 15/count 1 and reject register 15/count 2, register 16/count 1,
and an empty sampler range.

### Vertex storage versus semantic labels

The first color-pass shader exposed a separate correctness problem: upstream's
default input types and packed-normal conversions followed semantic names.
MC2 uses `TANGENT0` for float3 positions and `COLOR0` for unnormalized unsigned
bytes in this shader. It therefore compiled successfully while requesting the
wrong native input types.

The project patch keeps the original `UNLEASHED_RECOMP` specializations behind
that build flag. The default converter selects unsigned/signed integer input
types for unnormalized byte fetches and float inputs for normalized bytes and
the captured float formats. It uses the fetch instruction addressed by the
container metadata, checks that address, and does not inject Sonic's normal or
UV conversions based on a semantic name. Native input-layout decoding supports
the verified float32 and unsigned-byte formats, including mini-fetch offsets.

Synthetic converter checks cover the mismatched names/types, constant-only
fetches and out-of-range metadata. All 187 captured shaders still compile after
this correction (`out/native-renderer/shader-proof-gameplay-typed/results.json`).
The first color pass uses the same vertices, indices and position matrices as
the preceding depth pass. With the corrected shader and mixed float/byte input
layout, native GPU depth output matches the verified depth shader at all 921600
pixels, covering 295680 pixels with a maximum depth difference of zero. This
verifies that pass's vertex transformation and layout; color shading, other
packed formats and complete rendering parity are still unverified.

### Remaining semantic work

- `XenosRecomp/shader_recompiler.cpp`, `ShaderRecompiler::recompile(const uint8_t*, ...)`,
  asserts the `0x102A11xx` container signature and a nonzero constant-table offset.
  Passing raw dumps to this entry point is invalid. The new creation hooks
  recover intact boot shader inputs; extend their capture through gameplay before
  deciding whether any MC2 shaders require a raw-microcode/register interface.
- The pinned README lists mini vertex fetch as unimplemented. It is exercised
  by 62 captured shaders, so the native path must support its inherited fetch
  state and attribute selection. Skipping these operations is not acceptable.
- The pinned translator uses original declaration metadata to map fetch instruction
  addresses to input semantics. Recover the actual MC2 binding/declaration data;
  do not fabricate Sonic semantic assignments or infer them from a disassembly
  register number.
- Dynamic constant access is observed. A register-based path must preserve bounds
  behavior and distinguish shader stages. Runtime integer/boolean constants need
  a separate audit; absence from these lexical counts does not prove they are unused.
- Audit texture LOD/filter handling, packed vertex formats, and interpolator
  linkage against the captured corpus before compiling representative shaders.

The next gate is matched native draw comparisons with complete resource and
vertex-declaration state. Offline gameplay-shader compilation does not pass that
gate. Keep the current Xenos renderer active while these prerequisites are
implemented.

## Validation performed

### Texture fetch interpretation

The first color draw's BC3 diffuse texture uses piecewise gamma on RGB while
its BC1 normal texture does not. Native UNORM sampling alone misses that
conversion, and an sRGB view uses a different curve. The converter patch now
wraps texture samples with the captured component swizzle, selected gamma
channels and signed six-bit exponent adjustment. Weight queries and instruction
destination zero/one writes retain their existing behavior.

Each of the sixteen reflected fetch slots has a mode word in shared-constant
bytes 288..351 (`c18..c21`). `PackTextureFetchMode` validates the selectors and
range before packing bits 0..11 (swizzle), 12..15 (post-swizzle gamma mask),
16..21 (signed exponent), and 31 (enabled). Reserved selectors and gamma applied
to constant selectors are rejected. Zero disables interpretation for existing
movie uploads, which already expand/swizzle their source planes on the CPU.
The converter indexes modes by the original fetch slot, independently of the
native texture descriptor index. The existing `UNLEASHED_RECOMP` specialization
does not use this path.

The gamma helper is adapted from the BSD-3-Clause ReXGlue/Xenia
`PWLGammaToLinear`; attribution is retained in the patch and
[third-party notices](../../THIRD_PARTY_NOTICES.md). Like that reference backend,
it converts **after filtering**. This establishes a reference implementation to
compare with; it does not prove the original console's filtering order.

Build `mc2_native_fetch_check` with the optional native probe enabled. Its
original synthetic shader can be compiled using the patched converter header:

```text
dxc -T cs_6_0 -E main -HV 2021 -I <XenosRecomp-checkout>/XenosRecomp native/tests/texture_fetch_check.hlsl -Fo <local-output>/texture-fetch-check.dxil
mc2_native_fetch_check <local-output>/texture-fetch-check.dxil
```

The test compares GPU fetch cases with a separate double-precision CPU
calculation: all byte values, points either side of curve boundaries, NaN and
infinity handling, channel selection, constant zero/one, exponent extremes,
bit-exact disabled bypass, and all sixteen shared-buffer slots. Maximum
relative error was 1.20e-7. The D3D12 debug layer was unavailable. The translated
movie draw remains byte-identical to its previous 1280 x 720 output, and the
persistent upload/presentation tests pass with the new shaders. All 187 captured
shaders still compile/link; forward/reverse patch checks reproduce the tested
sources. Live scene color draws, full sampler-state mapping and matched frame
comparisons remain unfinished.

The same check compares target-zero quantization against an independently
enumerated table of all 1024 representable 7e3 values. It exercises their
adjacent midpoints, float values on both sides of each rounding boundary, and
two-bit alpha. Pixel specialization bit 2 (flags 4) enables this conversion
after the alpha test on the non-Unleashed path. It currently serves opaque
copy passes with 7e3 storage. Existing movie variants use flags zero.

`tools/check_mc2_shaders.py` passes synthetic checks for stage-aware deduplication,
little-endian dump matching to an unaligned embedded container, feature counting,
absent reflection, truncated/overflowing metadata extents, and malformed dumps.
The actual corpus inventory and Disc 1 scan completed without changing their
inputs. The pinned XenosRecomp and its pinned submodules also built successfully
with Clang 19.1.5 on Windows x64, with DXIL support enabled. The local executable
is `out/native-renderer/xenosrecomp-build/XenosRecomp/XenosRecomp.exe`; its
no-argument usage smoke check passed. Subsequent capture, compilation and bounded
native checks are described above; complete native scene rendering remains unverified.
