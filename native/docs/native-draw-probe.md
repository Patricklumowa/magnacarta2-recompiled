# Native draw probe

`mc2_native_draw_probe` renders one local draw through plume/D3D12 and reads its
pixels back after a real GPU fence. It links no ReXGlue runtime or Xenos GPU
plugin. The game still uses its existing renderer; this executable is an offline
validation step toward replacing that renderer.

## Build

Enable `MC2_BUILD_NATIVE_DRAW_PROBE` in the native CMake configuration. Set
`MC2_PLUME_ROOT` to a local [plume](https://github.com/renderbag/plume) checkout
at `11926860e878e68626ea99ec88562ce2b8badc4f`, with its dependencies initialized.
Apply `native/patches/plume-1192686-readback.patch` there first. It fixes the
texture-to-buffer copy path's null destination-texture access, preserving
sample-location handling for either texture copy direction. CMake checks the
revision and patch. No Vulkan dependency or in-game backend is added by this option.

Build target `mc2_native_draw_probe`, then run it with `--help` for the explicit
input arguments. All captures, converted shader blobs, constants and output
images remain local. Never commit game-derived inputs.

## Supported inputs

- Four float vertices: position XY plus UV (16-byte stride), or position XYZ
  plus UV (20-byte stride), in explicit big- or little-endian order.
- A quad list or triangle strip. Pixel-space XY conversion is available only
  for a separately verified passthrough vertex shader; transforming shaders
  require a different viewport implementation.
- One texture, or three textures for the observed movie shader. Inputs are
  already decoded, tightly packed RGBA8/BGRA8 UNORM. Texture descriptors occupy
  consecutive indices; all use sampler zero with the explicit common filtering
  and address modes. Gamma and exponent adjustment must be resolved beforehand.
- A 512-byte little-endian shared constant upload, including texture indices
  and the verified half-pixel correction. Movie draws additionally require
  4096 bytes of little-endian pixel float constants at `b1, space4`.
- One full-size target, copy blending, no depth/stencil, no culling or alpha
  test, one sample, and full viewport/scissor. Other requested state is rejected.

The caller must establish that these choices describe the captured draw. The
probe does not silently infer a complete pipeline from an incomplete snapshot.
Its `--label captured` flag records input provenance, not a parity guarantee.

## Observed results

The synthetic check rendered the game's simple splash shader pair against a
four-color fixture: all 4096 pixels matched. The three-plane movie check used
an actual UP draw at guest caller `0x8276A6DC` (return address `0x8276A6E0`):
primitive 6, four vertices, stride 20. Its textures were format 2, untiled R8,
1280×720 and two 640×360 planes, with no gamma or exponent adjustment, linear
sampling, clamp addressing, and only base mip levels. Device `+6016` supplied
the pixel constants. The translated vertex shader passes position/UV through;
the recorded viewport-transform control at `+10572` was `0x400` (scale/offset
disabled). The native replay used the D3D integer-pixel-center correction.

The native result reproduced the Bandai Namco logo. An independent CPU bilinear
sampling and color-conversion calculation using the captured planes/constants
compared all 921600 pixels: 920198 matched exactly, and no channel differed by
more than one of 255 UNORM steps. This checks resource binding, vertex fetch,
constant upload and the shader arithmetic for this draw. It does **not** establish
matched Xenos framebuffer parity, final presentation, or correctness of other
shaders, scene rendering, effects, resolves or synchronization.

The D3D12 debug layer was unavailable on the test machine; the probe reports that
fact and enables validation when the layer is installed. No speed claim follows
from this offline draw. Remaining work is native presentation, live resource
lifetime and upload handling, full pipeline-state decoding, and gameplay coverage.
