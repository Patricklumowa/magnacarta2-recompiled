# Native indexed depth checkpoint

The standalone native renderer can execute the first captured indexed scene
draw through D3D12. This is an offscreen depth-pass checkpoint, not native menu
or gameplay support. Live native boot still stops at indexed draws while the
game's clear, surface lifetime and composition boundaries are being connected.

`NativeDevice` owns one device, queue, allocator/list and fence. Movie and depth
pipelines can share it. Submission completion checks the real fence and device
removal before advancing the common counter. Calls must be serialized. The
depth pipeline retains its target and upload allocations; it never clears as a
side effect of drawing. Readback is optional.

## Vertex inputs

`ReadVertexInputs` bounds the shader container, metadata and instruction ranges.
It handles the verified 32-bit float formats, byte order and mini fetches that
inherit a preceding full fetch's stream and stride. Unsupported formats,
relative addressing, scaling, selectors and buffer extents fail explicitly.

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
```

The synthetic checks use the actual translated depth shader with controlled
geometry and identity constants. They verify indexed start/count, malformed
ranges, missing input signature rejection, normal and reversed depth ranges,
depth testing, retained depth between draws, explicit clears, scissoring,
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

This comparison validates the captured geometry and selected native state. It
does not establish the game's preceding clear value or match a full Xenos frame.
The native target currently uses D32_FLOAT; console 24-bit depth quantization,
stencil, MSAA, other input formats/streams, resolves and scene composition are
not implemented. Reversed viewport depth worked on the tested GPU/driver; older
D3D12 drivers may need a shader-based depth conversion. Do not remove effects
or silently pass unsupported draws to advance the boot checkpoint.

Microsoft documents inverted viewport support in its
[D3D12 specification](https://microsoft.github.io/DirectX-Specs/d3d/VulkanOn12.html#inverted-viewports).
