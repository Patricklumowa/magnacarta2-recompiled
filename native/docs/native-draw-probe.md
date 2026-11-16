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
matched Xenos framebuffer parity or correctness of other
shaders, scene rendering, effects, resolves or synchronization.

The D3D12 debug layer was unavailable on the test machine; the probe reports that
fact and enables validation when the layer is installed. No speed claim follows
from this offline draw. Remaining work is in-game native presentation, live resource
lifetime and upload handling, full pipeline-state decoding, and gameplay coverage.

## Native window presentation

`QuadRenderer::SetWindow` attaches a caller-owned Windows HWND. `Present` draws
directly into a two-buffer D3D12 swapchain at the current client size, preserving
the source aspect ratio with black bars. Resizing retains the device and pipeline;
zero-size and minimized windows submit no work. Each submission waits for its
actual GPU fence before uploads or framebuffers can be reused. Normal presentation
does not copy pixels back to the CPU. The optional readback argument is for checks.

Add `--present-frames N` to a probe invocation to open a native window and submit
that many frames. `--window-width N --window-height N` override the initial client
size. The usual output file records the first submitted backbuffer, at that
window size. Closing the window ends the run. These counts describe successful
DXGI submissions, not distinct simulation updates or measured monitor scanouts.

A captured 1280x720 movie draw was submitted 120 times through this path; its
first backbuffer was byte-identical to the verified offscreen image. This is
native presentation of a captured draw, not a live game backend. The working game
still uses Xenos presentation until its startup and synchronization are replaced.

## Bounded live comparison

`MC2_ENABLE_NATIVE_VIDEO_CHECK` builds an optional comparison hook using the same
`native_quad` renderer. It defaults off at build time and at runtime. The hook
copies live movie inputs before the original UP draw, performs a native offscreen
draw, waits for its GPU fence, and records the result. The original guest draw
still executes. This does not replace the in-game renderer or its command ring.

Prepare a local shader directory containing the validated movie
`vertex-shader.container`, `pixel-shader.container`, `vertex-declaration.bin`,
and their translated `vertex.dxil` / `pixel.dxil`. The pixel binary must have
specialization flags zero. No such files are distributed by this project.
Set these cvars using absolute paths:

```text
--mc2_native_video_check_dir=<new output directory>
--mc2_native_video_shader_dir=<verified local shader directory>
--mc2_native_video_check_skip=180
--mc2_native_video_check_count=2
```

The check requires the observed caller and ABI, exact bound shader container
bytes, the verified declaration elements, and the three-plane texture/sampler
layout. A mismatch or read/I/O failure disables further native checks and logs
the reason while the original renderer continues. An empty output setting does
nothing. Count is clamped to 1–4, and each process creates a fresh session.
Successful frames include raw input/output files and a `metadata.json` marker
written last; ignore a frame directory without that marker. Inputs are copied,
but guest-memory reads across multiple resources are not an atomic snapshot.

The first live run rendered movie draws 180 and 181 and exited cleanly. Both
native images passed the independent CPU comparison with the same one-channel-step
bound as the offline result. A deliberately mismatched shader was rejected without
closing the game. The capture-wrapper context/memory checks still pass after
sharing its bounded guest reader with this hook.

This path remains a verification tool that synchronously reads pixels back.
`QuadRenderer` retains its D3D12
device, queue, pipeline, descriptors, uploads, textures and readback buffer for
one fixed layout. It waits for actual GPU completion before reusing them on the
next draw; changed pipeline or texture extents require a new renderer. A returned
`completed_submission` counts successful fenced draws and is also recorded in
the live check manifest. It does not write the original game's progress fields.
Leave the comparison path disabled for gameplay
and performance measurements. It never enables the D3D12 debug layer after the
game's device has been created. The eventual streaming renderer needs persistent
resources for other draw families, complete state handling, game presentation,
and retirement tied to the real GPU queue.

`mc2_native_quad_check` takes the verified local movie VS DXIL and flags-zero PS
DXIL as its two arguments. Its synthetic inputs first exercise four submissions on one
renderer: texture and pixel-constant updates, a vertex update moving the quad
offscreen, then a valid draw after rejected descriptor/truncated-upload inputs.
It then uses a hidden HWND to check every backbuffer pixel during resize,
letterboxing, zero-size suspension, detach/reattach, and presentation without
readback. Ten completed submissions are checked, including an offscreen draw
between window presentations. This catches stale uploads, missing command-state
rebinding and premature resource reuse; the shaders and test binaries remain local.
