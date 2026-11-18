# Native indexed depth checkpoint

The standalone native renderer can execute the first captured indexed scene
draw through D3D12. An opt-in live path now executes that draw and its preceding
two-sample depth clear on the same device as the movies, without loading Xenos.
The following HDR clear and both tiles' indexed color draws and resolves also
run natively. Native menu and gameplay rendering, general surface lifetime,
and scene composition remain unfinished.

`NativeDevice` owns one device, queue, allocator/list and fence. Movie and depth
pipelines can share it. Submission completion checks the real fence and device
removal before advancing the common counter. Calls must be serialized. The
depth pipeline retains its target and upload allocations; it never clears as a
side effect of drawing. Readback is optional.

## Vertex inputs

`ReadVertexInputs` bounds the shader container, metadata and instruction ranges.
It handles the verified 32-bit float and unsigned-byte formats, byte order and
mini fetches that inherit a preceding full fetch's stream and stride. Unsupported formats,
relative addressing, scaling, selectors and buffer extents fail explicitly.

Input types follow fetch formats rather than semantic names. The first color
shader stores float3 positions under `TANGENT0`, and integer byte attributes
under `BINORMAL0`, `NORMAL0` and `COLOR0`. The patched converter's signature and
native input layout now agree on these types; see
[shader feasibility](shader-feasibility.md#vertex-storage-versus-semantic-labels).

The first draw has a constant-only full fetch followed by two mini fetches. Its
DXIL signature still includes the unused `POSITION0` input. D3D12 rejects the
pipeline if that signature element is absent, even though the shader does not
read it. A zero-stride dummy stream supplies constant-only signature elements;
the shader still writes its own literal components. Actual positions come from
the `COLOR0` semantic at byte offset 0, and UVs from `TEXCOORD0` at offset 24.
Names alone cannot establish these buffer mappings.

The pinned plume pipeline-validation patch returns null when D3D12 rejects a
pipeline. Without it, a non-null wrapper contains a null native PSO, causing a
later draw to crash instead of reporting the failed configuration.

## Validation

Build with `MC2_BUILD_NATIVE_DRAW_PROBE=ON`, applying both documented plume
patches. The optional check consumes local, game-derived inputs:

```text
mc2_native_depth_check <first-depth-VS.container> <translated-VS.dxil>
mc2_native_depth_check <first-depth-VS.container> <translated-VS.dxil> <draw-capture-directory> <output.f32>
mc2_native_depth_check <first-depth-VS.container> <translated-VS.dxil> <draw-capture-directory> <output.f32> <first-color-VS.container> <corrected-color-VS.dxil>
```

The synthetic checks use the actual translated depth shader with controlled
geometry and identity constants. They verify indexed start/count, malformed
ranges, missing input signature rejection, normal and reversed depth ranges,
depth testing, retained depth between draws, explicit full and partial clears, scissoring,
two-sample MSAA coverage and a maximum-depth resolve for diagnostic readback,
optional readback and two pipelines sharing one completion timeline. Movie
upload/presentation tests also pass after the shared-device refactor. The D3D12
debug layer was unavailable during these checks; the tests report that fact.

The captured draw uses 24 vertices at stride 40, 36 32-bit indices, and selects
indices 6 through 23. It has no pixel shader, color target or vertex declaration.
Using its constants, a 1280 x 720 viewport with depth endpoints 1 and 0, and an
explicit test clear to zero, the native output covers 295680 pixels. An
independent CPU calculation of transformation, homogeneous clipping, culling,
raster coverage and depth covers exactly the same pixels. The maximum depth
difference on covered pixels was 2.37e-7. Game-derived artifacts stay ignored.

This comparison used a single-sample target. The captured surface actually has
two samples, which the renderer now also supports. The optional captured-input
check retains that sample count and resolves the greatest depth for inspection;
this diagnostic resolve does not implement the game's resolve contract.

Live native tracing subsequently observed the preceding full-surface clear:
`sub_8238AD80`, flags `0x30`, depth zero, a 1280 x 720 rectangle, matching viewport,
no enabled scissor and a two-sample depth surface. With the optional depth shader
directory configured, native boot replaces this clear and the following indexed
draw and the next HDR tile clear. The optional color path continues into the
first tile's indexed color draw. Indexed capture runs before the native draw,
preserving live inputs.

These comparisons do not match a full Xenos frame. The native target currently
uses D32_FLOAT; console 24-bit depth quantization, stencil, other input
formats/streams, game surface resolves and scene composition are not implemented.
Reversed viewport depth worked on the tested GPU/driver; older
D3D12 drivers may need a shader-based depth conversion. Do not remove effects
or silently pass unsupported draws to advance the boot checkpoint.

Microsoft documents inverted viewport support in its
[D3D12 specification](https://microsoft.github.io/DirectX-Specs/d3d/VulkanOn12.html#inverted-viewports).

## Live depth path

In addition to the [native movie boot flags](native-bootstrap.md), set:

```text
--mc2_native_boot_depth_shader_dir=<local verified depth shader directory>
```

The directory must contain the captured `vertex-shader.container` and translated
`vertex.dxil` verified by the checks above. The runtime requires an exact shader
container match, the known input layout, matching cached fetch/resource bytes,
32-bit indices, zero vertex-base offset, the verified raster/depth state, full
viewport and the same cleared surface allocation. Other states fail explicitly.

The observed stencil clear is zero, but native stencil storage is not implemented;
every stencil-tested draw is rejected before submission. No stencil-dependent
effect is allowed to continue with missing data. The initial surface cannot yet
be replaced or retired independently. The next color pass binds 1280 x 512
physical surfaces for a 1280 x 720 logical scene. Its initial color clear is now
handled as described below; depth sharing and subsequent resolves are pending.

`mc2_native_scene_check` reconstructs a captured draw in bounded relocated guest
memory and checks geometry, endian conversion, unchanged input state, clear/draw
surface identity and rejection of stencil, color targets, different fetches,
vertex-base offsets, clipping and declaration changes. It accepts a captured
draw directory and the local verified depth shader directory as its two arguments.

## Tiled HDR color clear

The engine's begin-frame routine (`0x826DA0B8`) configures two tiles through
`0x8237BB88`: `[0,0,1280,512]` and `[0,512,1280,720]`. The device retains their
logical extent at offsets 13188/13192 and their attachment bindings at
12736..12752. The engine then selects predicate `2`, the first tile, before
clearing color. End-frame (`0x826DA200`) selects `2 << (tile * 2)` for subsequent
per-tile resolve calls. These values were checked against captured state and
the live clear boundary, including the explicit first-tile predicate.

`ReadSceneColorClear` verifies this exact layout, attachment identity, sample
count, viewport, rectangle and predicate. It maps the first clear to
`[0,0,1280,512]` on a native 1280 x 720 HDR target. It does not clear the lower
tile early or infer the scene extent from viewport dimensions alone.

The observed color format is 7e3 RGB with two-bit normalized alpha. Native
storage uses RGBA16_FLOAT; currently only the observed zero clear is accepted,
which is exact in both formats. General blending, tiled draw ordering, depth
sharing across tile aliases and resolve coverage beyond this pass remain unfinished.

`ColorTarget` retains its two-sample allocation on the shared native device and
completes its real fence after the clear. Diagnostic readback is optional and
rejects undefined pixels in a partially initialized surface. The synthetic
`mc2_native_color_check` verifies HDR values above one, full and partial clears,
independent retained targets, one/two/four-sample resolves and fence completion.
Passing an optional third directory (the captured first color draw) to
`mc2_native_scene_check` verifies the tile selection and rejected changes to
the layout, predicate, scissor, clear color and cached attachment state.

A live run with the Xenos DLL physically unavailable completed the depth draw,
the HDR tile clear, and both tiles' color draws and resolves. Frame
composition remains unfinished.

## Compressed scene textures

`SampledTexture` uploads native-order BC1/BC3 blocks into an immutable D3D12
texture with every supplied mip level. It keeps compression, aligns transfer
rows to 256 bytes and subresources to 512 bytes, then completes a real fence
before releasing the temporary upload. The texture remains ready for shader
reads; optional diagnostic readback restores that state after copying.

Top-level extents must be multiples of four and at most 8192 per dimension;
transfer storage is bounded to 64 MiB. Smaller mip levels may have partial
blocks. Input format, mip count and every payload extent are checked before
allocating or submitting GPU work. Swizzle, filtering, exponent adjustment and
the game's gamma conversion are not performed by this storage component.
The patched shader's [texture fetch interpretation](shader-feasibility.md#texture-fetch-interpretation)
handles swizzle, gamma and exponent after sampling; it has a separate GPU check.

```text
mc2_native_texture_check
mc2_native_texture_check <first-color-draw-capture>/textures
```

The check independently queries D3D12's copy footprints and verifies GPU
roundtrips for both formats, non-square sizes, partial edge blocks, tiny mip
tails, independent retained textures and invalid inputs. The captured first
color draw's two 64 x 64 textures each have seven mip levels; all fourteen
uploaded levels read back byte-for-byte correctly. The D3D12 debug layer was
unavailable. This validates storage, not filtering, pixel shading or a complete
native color pass. No game-derived texture payload is included in the repository.

Transfer alignment follows Microsoft's
[texture upload documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data).

## Indexed color replay

The offline check submits the first color draw with its captured indexed
geometry, shaders, constants and two block-compressed textures. It runs on
the same device, queue and depth attachment as the preceding prepass. Both
passes preserve the observed viewport offset of (-0.25, +0.25) pixels. The
color pass maps the observed copy blend, full color write, disabled alpha test
and two-sample HDR target. It selects the first tile's cleared rectangle for
this diagnostic replay.

Put the first color draw's `vertex-shader.container` and `pixel-shader.container`
in a local directory with `vertex.dxil` and `pixel-7e3.dxil`. Compile with the
patched XenosRecomp converter and link the pixel shader with specialization
flags 4, which enables 7e3 color and two-bit alpha storage at target zero.
Use the schema 4 draw capture so the two textures include every raw mip.

```text
mc2_native_scene_check <depth-capture> <depth-shaders> <color-capture> <color-shaders> out/color-diagnostic.rgba16f
```

In the first tile, 29,417 pixels receive the color pass, matching the retained
depth coverage. The recorded lighting constants yield zero RGB and saturated
alpha, with partial coverage resolved to half alpha. A second pass with diagnostic lighting produces HDR
color from the captured textures, and toggling the diffuse gamma interpretation
changes the red channel in all 29,417 covered pixels. The check also verifies
that writes stay within the tile, uncleared attachments reject draws and a closer
depth clear rejects the geometry. The output file is the diagnostic lighting
readback. The D3D12 debug layer was unavailable during validation.

The pixel shader rounds each RGB channel to one of the 1024 representable
7e3 values before writing the float16 target. Float16 stores these RGB values
exactly and approximates the intermediate one-third and two-thirds alpha
levels. This applies to copy passes. Blending needs separate handling.
Float24 depth and console sample locations still need validation.

The optional live path consumes the same shader directory and reads the actual
texture bytes from guest memory. The first tile begins at (0, 0) and its depth
descriptor has the same base, pitch, sample count and format as the full depth
prepass. The native draw tests and updates that retained depth attachment,
scissored to the first tile. When the lower color resolve is requested, the
backend clears the lower color region and replays the same opaque draw against
the retained lower depth. It verifies that both shaders receive the same
position transforms, geometry and viewport. The captured lower replay produces
identical color and maximum depth values with either retained or freshly
cleared depth.

`--mc2_scene_resolve_trace=true` logs the first eight tiled resolve calls and
the first eight untiled scene resolve calls on a native boot development build.
The reference renderer observed two calls per
tile, at masks `2` and `8`. Flags `0xf4000100` average color samples into a
1280 x 720 RGBA16F texture and request a zero color clear. Flags `0x214` copy
depth sample zero into a separate 24-bit float depth/stencil texture and
request a zero depth/stencil clear. Each tile uses its rectangle's origin in
the destination. The reference run exited cleanly after collecting both pairs.

To enable the first color resolve, put `color-resolve.dxil` next to the color
shader inputs. Compile the project's resolve shader with:

```text
dxc -T cs_6_0 -E main -HV 2021 native/src/native_color_resolve.hlsl -Fo <color-shaders>/color-resolve.dxil
```

The native resolve validates each tile's predicate, source, rectangle,
destination origin, RGBA16F texture descriptor and zero clear. It averages the
two samples, applies the destination exponent of -3 to all four channels,
writes the retained single-sample texture and clears the source rectangle.
Both tiles write into the same allocation at their respective offsets.
Both operations complete on one native fence. The guest routine still updates
CPU resource bookkeeping after the native fence.

The offline replay compares every resolved channel with the D3D12 hardware
average scaled by 1/8, and checks that the source clear stays within the tile.
To enable the first depth resolve as well, compile this file next to
`color-resolve.dxil`:

```text
dxc -T cs_6_0 -E main -HV 2021 native/src/native_depth_resolve.hlsl -Fo <color-shaders>/depth-resolve.dxil
mc2_native_depth_check <depth-VS.container> <depth-VS.dxil> <color-shaders>/depth-resolve.dxil
```

The native depth target uses distinct depth and shader views of its R32
storage. The depth resolve reads sample zero and stores the decoded 20e4
value in an R32_FLOAT texture, then clears the selected source rectangle.
The game requests a zero stencil clear and the verified draw has stencil
disabled. Native draw-time depth testing still uses D32_FLOAT.

The diagonal test verifies that sample-one-only pixels stay clear in this
copy, while the diagnostic maximum includes them. The captured scene check
also verifies float24-representable output and scoped depth clears. It checks
that the lower tile contributes 266,032 nonzero depth pixels and that both
resolved textures retain the upper tile unchanged. A live run with the Xenos
DLL physically absent completed all four copies and cleared their sources.
The guest then retired its staging chunks and wrote the completion marker on
the CPU after the final native fence. The separate single-sample surface
then receives a native zero stencil clear with depth writes disabled.

## Single-sample depth restore

The next pass samples the full resolved depth texture and writes depth into the
single-sample D32/S8 attachment. It uses the game's direct user-memory draw
with three rectangle corners. The native bridge snapshots the draw state at
allocation, reads the vertices after the guest commits its staging cursor,
and expands the rectangle into two triangles. The original shaders apply the
depth bias. Color writes are masked and stencil uses Always with Keep.

To enable this pass, put its verified `vertex-shader.container`,
`pixel-shader.container`, `vertex.dxil` and `pixel.dxil` in the `depth-restore`
subdirectory of the color shader directory. Use the default specialization
for the pixel shader. The captured scene check uses this subdirectory when
available. Its D3D12 readback checked both planes across all 921,600 pixels
and verified the sampled depth, bias and stencil preservation. The live run
completed the restore without Xenos and reached the indexed fullscreen draw.

## Fullscreen HDR passes

The indexed user-memory draw at `0x826D9CC0` samples the completed HDR
resolve and writes all four channels into a single-sample 1280 x 720 HDR
attachment. The bridge waits for the guest's vertex and index writes and
validates the fullscreen geometry before replaying it. The game subsequently
clears both planes of the single-sample depth/stencil attachment. The grading
draw at `0x827B061C` expands its triangle fan to a triangle list and writes
RGB while retaining the copy's alpha.

Put the verified shader inputs in `fullscreen` and `color-grade` subdirectories
of the color shader directory. Each has `vertex-shader.container`,
`pixel-shader.container`, `vertex.dxil` and `pixel-7e3.dxil`. Both pixel shaders
use specialization 4. The captured scene check can replay these passes with
diagnostic grading constants and nonzero source alpha. It verifies coverage
and exact alpha preservation across all 921,600 pixels.

The fullscreen resolve at `0x827B3694` writes the graded image into the same
native allocation used by the tile resolves. It scales all four channels by
1/8 and preserves the graded source. Compile the single-sample variant into the
`fullscreen` shader directory to enable it:

```text
dxc -T cs_6_0 -E main -HV 2021 -D MC2_SINGLE_SAMPLE=1 native/src/native_color_resolve.hlsl -Fo <color-shaders>/fullscreen/color-resolve.dxil
```

The captured scene check verifies that every exported channel has the expected
float16 value and the source survives the copy. A live boot without Xenos
completed this resolve and cleared the 322 x 182 target for blur and bloom.
Native storage for this single-sample target is RGBA16_SNORM. The color
check verifies both signs and zero using GPU readback.

## Blur and bloom

The first blur and bloom draw uses the native depth resolve and the graded
HDR color texture. It runs over the 320 x 180 interior of the cleared target.
The pixel shader uses specialization 8 of the repository's XenosRecomp
patch to encode the signed fixed-point range into the native SNORM target.
Put `vertex-shader.container`, `pixel-shader.container`, `vertex.dxil` and
`pixel-fixed16.dxil` in the `bloom` subdirectory of the color shader directory.

The D3D12 replay verified the one-pixel border and changed the input textures
independently. The depth change altered 18,560 output alpha values and the
color change altered all 57,600 interior red values in the diagnostic. A live
boot without Xenos completed this draw with the original game parameters.

The game then resolves the signed target into a 322 x 182 RGBA16_UNORM
texture. The native dispatch decodes the signed range, rounds to unsigned
16-bit values and retains the source. The GPU replay checks the conversion
with the bloom output and signed test values. Compile the shader into `bloom`:

```text
dxc -T cs_6_0 -E main -HV 2021 -D MC2_SINGLE_SAMPLE=1 -D MC2_FIXED16_TO_UNORM=1 native/src/native_color_resolve.hlsl -Fo <color-shaders>/bloom/color-resolve.dxil
```

The two subsequent filter draws sample this texture with bilinear clamp
sampling. Each writes to the retained signed target and resolves into the
same separate UNORM texture before the next draw samples it. Put the filter
shader containers and compiled shaders, using the same filenames as the
initial bloom pass, in `bloom/filter`. The GPU check uses a single-pixel
input and verifies the weighted samples and the spread after the second draw.
A live boot without Xenos completed both filters and all three bloom resolves.

## Depth of field composition

The fullscreen composition samples the completed depth and HDR textures with
point sampling and the filtered bloom texture with bilinear sampling. It
writes the original four-channel output to the existing HDR target. The
game then resolves it with the same exponent and destination used by the
grade pass. Put the corresponding containers, `vertex.dxil`, and
`pixel-7e3.dxil` in `fullscreen/compose`.

The GPU replay varied each of the three sources and matched the expected
quantized RGBA output at every pixel. The live run without Xenos completed
the composition and resolve before reaching the back-buffer draw.

## Back-buffer color pass

The back-buffer pass reads the resolved HDR image and applies the game's
color scale, overlay and gamma constants to a native 1280 x 720 RGBA8
surface. Put its shader containers, `vertex.dxil`, and `pixel.dxil` in
`fullscreen/back-buffer`. GPU readback matched the expected four channels
at every pixel of the diagnostic. Live boot completed the same pass, then
reached the first alpha-blended game overlay draw.

## Back-buffer overlays

The game's overlay draws reuse the 48-byte indexed vertex layout and feed
the original color and gamma constants to the unspecialized pixel shader.
The renderer samples the original RGBA8 and BC3 textures with the verified
base-level sampler and blends RGB with source alpha while retaining the
destination alpha. Put the shader containers, `vertex.dxil`, and `pixel.dxil`
in `fullscreen/overlay`.

The GPU check verifies the RGBA8 upload and blend result. The live run
completed all seven overlay draws before the display resolve, including
both 108-index atlas batches.

## Scene presentation

The display resolve validates a full, same-size RGBA8 source and destination.
The native renderer retains the completed attachment until the guest swaps
that destination. A D3D12 draw samples it directly into the same DXGI
swapchain used by the startup movies and preserves the source aspect ratio
at the window's client size.

Compile the presentation shaders into the color shader directory:

```text
dxc -T vs_6_0 -E main -HV 2021 -D MC2_PRESENT_VERTEX=1 native/src/native_present.hlsl -Fo <color-shaders>/fullscreen/present/vertex.dxil
dxc -T ps_6_0 -E main -HV 2021 native/src/native_present.hlsl -Fo <color-shaders>/fullscreen/present/pixel.dxil
```

A live run with the Xenos DLL absent completed the first post-movie scene
swap at a 2560 x 1600 window extent. It reached the second scene's depth
clear, which still requires per-frame state reset.
