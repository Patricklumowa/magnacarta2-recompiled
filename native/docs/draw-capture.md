# Selected draw capture

The default renderer can capture a bounded set of draw inputs for local graphics
investigation. Capture runs immediately before the original guest draw function;
the wrapper then calls the original with the same context and guest memory. It
does not replace rendering. Captures contain game data and must stay local.

Set these development cvars before launching:

```toml
mc2_draw_capture_dir = "out/local-draws"
mc2_draw_capture_hook = "up"
mc2_draw_capture_skip = 0
mc2_draw_capture_count = 1
```

An empty directory setting disables capture. The hook filter accepts `all`,
`up`, `indexed`, and `nonindexed`; its default is `all`. Skip counts matching
calls only, from zero. Count is clamped to 1–32. A process creates one fresh
session directory and captures at most 32 draws and 256 MiB, including metadata.
Each individual artifact is limited to 16 MiB. Existing capture sessions are not
overwritten. The implementation currently checks readable pages with Windows
`VirtualQuery`; other hosts record unavailable guest reads.

## Hook ABI

These interfaces come from the supported image's decoded PPC instructions and
generated C++, as detailed in [graphics-map.md](graphics-map.md):

| Hook | Address | Inputs beyond `r3 = device` |
| --- | --- | --- |
| `nonindexed` | `0x828652A0` | `r4` primitive, `r5` start vertex, `r6` vertex count |
| `indexed` | `0x82865690` | `r4` primitive, `r5` start index, `r6` index count |
| `up` | `0x82864D40` | `r4` primitive, `r5` vertex count, `r6` source pointer, `r7` byte stride |

The UP wrapper copies `r5 * r7` source bytes. Its multiplication, pointer range,
and artifact limit are checked before reading. The separate indexed-UP entry
point is not intercepted by this capture.

## Schema version 2

Each `session-<timestamp>-<suffix>/draw-<ordinal>/metadata.json` contains the hook
name/address, matching-call ordinal, device address, link register, and numeric
`r3`–`r10` values. `artifacts` lists relative filenames, capture status, origin,
guest/physical addresses, byte size, and a hexadecimal FNV-1a-64 content hash.
Hashes are identifiers for local comparison, not cryptographic integrity proofs.

Successful files are:

| File | Contents |
| --- | --- |
| `device.bin` | 24064 bytes of pre-call device state, including float constants, viewport/state words and fetch constants |
| `gpu-progress.bin` | 96 bytes at device `+10896`, including the completed marker at block `+0` and ring read-pointer writeback at block `+60` |
| `gpu-auxiliary.bin` | 32 bytes at device `+10900`; recorded without assuming all fields' meanings |
| `vertex-shader.container`, `pixel-shader.container` | Bound shader virtual header/body plus its separate physical payload, reconstructed into a contiguous container |
| `vertex-declaration.bin` | Resource header and declared elements: `56 + 12 * count`, with type and count checks |
| `up-vertices.bin` | UP source bytes, without endian conversion |
| `vertices/slot-NN.descriptor.bin` | First 32 bytes of each bound vertex buffer descriptor |
| `index-buffer.descriptor.bin` | First 32 bytes of the bound index buffer descriptor |
| `textures/slot-NN.fetch.bin` | First 26 slots' six raw big-endian fetch words from device `+1152+24*slot`, including slots without a resource pointer; this limit is not a claim of complete texture-state coverage |
| `textures/slot-NN.descriptor.bin` | First 52 bytes of each bound texture resource |
| `textures/slot-NN.base.bin` | Supported slots 0–2's exact base-level physical spans, including any intervening row/tile padding |
| `textures/slot-NN.rgba8.bin` | Slots 0–2 decoded to tightly packed RGBA8 rows, endian conversion and fetch swizzle applied |
| `targets/color-N.descriptor.bin`, `targets/depth.descriptor.bin` | First 48 bytes of bound surface descriptors |

Device pointers and words retain guest big-endian representation. Shader
container virtual and physical spans are validated independently and read through
SDK `GuestPtr`, including the Windows high-address mapping adjustment. Resource
headers may have been modified by initialization; the reconstructed shader
origin is explicitly recorded as `binding_reconstructed`.
For those shader artifacts, `physical_address` is the resource's guest pointer
to the physical segment; for texture artifacts it is the raw GPU physical address.

Vertex declarations are bounded using their observed initializer: resource type
5, count at `+24`, 12-byte elements at `+52`. The initializer counts an input
array terminated by stream 255. Capture reads the resource's allocation extent
and imposes a conservative 256-element cap.

Texture decoding currently supports non-array 2D format 2 (`8`) and format 6
(`8_8_8_8`) unsigned/gamma fetches. Format 2 replicates its single component
before the fetch swizzle, including the movie's `RRR1` selection. The SDK texture layout and tiled offset
functions determine its physical span. Its 29-bit physical address is mapped
through guest alias `0xA0000000 + physical_address`; the entire span must remain
inside physical memory and below the artifact cap. Other formats are recorded
as unsupported rather than interpreted as RGBA8.

The top-level `textures` array has three entries, indexed by slot. Each contains `slot`, `format`, `width`, `height`,
`physical_address`, `source_bytes`, `row_pitch_bytes`, `tiled`, `offset_x`,
`offset_y`, `endianness`, `swizzle`, `gamma_mask`, `exp_adjust`, `mip_min_level`,
`mip_max_level`, `rgba8_decoded`, and six numeric host-endian `fetch_words`.
An entry is null if its fetch cannot be described. Sampler fields remain encoded in
the fetch words. **Gamma conversion and exponent adjustment are not applied to
the RGBA8 output**; a replay must interpret those fields explicitly. A valid
description alone does not establish that either payload file was captured:
consumers must check artifact statuses.

## Deliberate limits

Every manifest reports `replayable: false` and lists `missing_for_replay`.
Ordinary buffer payload extents have not been established; only their
descriptors are captured. Only slots 0–2's supported base levels are captured;
other textures and mip levels need more work. Color/depth attachment contents,
prior draw dependencies, full native pipeline interpretation, and shader
constant/resource binding semantics still require validation. Device state is
a pre-call snapshot and may contain dirty words that the original draw flushes.
No complete-frame replay or native-renderer correctness is implied.

Selected capture performs synchronous local disk writes and can stall those
draws. It is a diagnostic run, not a performance measurement. Guest memory may
also change on another guest thread between reads; this is not an atomic process
snapshot. Readability is checked before copying, but concurrent unmapping is
not synchronized with the capture. A capture I/O failure disables further
capture without skipping the original draw. A partially written draw without
metadata is incomplete and must not be consumed.

`draw_capture_check` checks selection and malformed extents, including 32-bit
wrap and the high-address mapping discontinuity. Its fake originals compare
the full incoming PPC context and guest bytes, modify sentinel return state,
and check that all three wrappers preserve those changes. Its texture fixture
reserves the real A-range virtual alias while committing only fixture pages,
and verifies both captured physical bytes and decoded row data, including the
three separate movie texture slots. See [native-draw-probe.md](native-draw-probe.md)
for the standalone D3D12 draw experiment.
