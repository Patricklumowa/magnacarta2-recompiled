# MC2 graphics hook evidence

This is a partial static map for the supported executable, not a completed
native renderer specification. No runtime hook coverage is claimed here.
Recompiled source and decoded executable were inspected on 2026-11-16.

Supported decoded, page-padded image SHA-256:
`f22261518998a368511f33d533dd8d07e1844efcb7c618e49817dc5c5d936548`.
Both disc executables were previously found to have that decoded image. Validate
the installed input rather than assuming this remains true for another dump.

## Reproduce the evidence

Run from the repository root after local code generation:

```powershell
python tools/map_mc2_graphics.py --xex 'Magna Carta 2 Disc 1/default.xex'
python tools/check_graphics_map.py
```

The inventory is written to ignored `out/native-graphics/graphics-map.json`.
It contains source hashes, current generated-file line numbers, direct callers,
outgoing direct calls, and video import call sites. Pass `--function sub_HEX` to
include an additional candidate. Generated chunk numbers can change; locate
functions by symbol and guest address, not by a permanent chunk filename.

The graph index excludes generated game code. This inventory fills that direct
call coverage gap; indirect calls, inline state writes, and live resource usage
still need separate investigation. A source-file hash does not establish that
the generated code corresponds to the supplied executable.

## Verified shader creation and binding

Names below describe observed behavior, not original debugging symbols.
Integers in guest memory are big-endian. All offsets are bytes.

| Address | Observed role and entry ABI | Output/side effects |
| --- | --- | --- |
| `0x82372098` | Container layout helper; r3 = container, r4 = 16-byte output | Output words: container pointer, virtual allocation size, physical-data pointer, physical byte count. No useful return value established. |
| `0x82372120` | Pixel-shader creation; r3 = contiguous container | r3 = allocated resource or zero. Copies virtual container to resource+40 and physical bytes to a separate allocation. |
| `0x823724C8` | Vertex-shader creation; r3 = contiguous container | r3 = allocated resource or zero. Copies virtual container to resource+872 and physical bytes to a separate allocation. |
| `0x82372210` | Pixel-shader binding; r3 = device, r4 = shader resource or zero | Writes device+12700; updates dirty flags and constant definitions. Do not replace with only the pointer store. |
| `0x823725B8` | Vertex-shader binding; r3 = device, r4 = shader resource or zero | Writes device+12704; updates dirty flags and constant definitions. |

Both creation routines read virtual size at container+4 and physical size at
container+8, then use `container + virtual_size` as the physical source. A
pre-call capture can therefore preserve the original contiguous container using
`virtual_size + physical_size`, provided address mapping, overflow, size limits,
and readability are checked before copying. Capture must preserve the original
PPC context and still call the original implementation.

The stage identity has independent caller evidence in `sub_8248C6F8`:

- The profile string `vs.2.0` at `0x8217EAF4` and entry name
  `SplashVertexShader` at `0x8217EAFC` feed the compile call, followed by
  `sub_823724C8` at instruction `0x8248C818`.
- The profile string `ps.2.0` at `0x8217EB10` and entry name
  `SplashPixelShader` at `0x8217EB18` feed the second compile call, followed by
  `sub_82372120` at instruction `0x8248C880`.
- Other direct creation callers are `sub_826DA3A0` and `sub_82769E00`. The map
  records four pixel creation calls and three vertex creation calls statically;
  these counts say nothing about runtime shader diversity.

Resource initialization is also visible independently:

- Pixel initializer `sub_823720E8` receives resource r3 and physical pointer r4;
  writes resource type 7 at +0, reference count 1 at +4, and physical pointer at
  +24. The creator later ORs ownership flag `0x00100000` into the first word.
- Vertex initializer `sub_823723D0` receives the same two arguments; writes
  resource type 6 at +0, reference count 1 at +4, and physical pointer at +32.
  It also builds vertex-fetch information. These are MC2 values: they are not
  Unleashed's host `ResourceType` enum.
- Existing `sub_82371D88` takes device/bank r3, first float4 index r4, source r5,
  and float4 count r6. Destination starts at `r3 + (r4 + 120) * 16`; it copies
  bytes and updates dirty state. The existing fast hook retains original tail
  iterations to preserve PPC register results. Its 49 static direct calls do
  not establish that all constants pass through it.

Useful decoded byte-range hashes (end exclusive, including alignment bytes):

| Range | SHA-256 |
| --- | --- |
| `82372098..823720E8` | `cbd33bc9ea13f7c37b723c0503d0dcd80ad257ea24ef4faa2dbe66344f08b8ff` |
| `82372120..82372210` | `2c5a8763078eddec92f58e90d991383a57d34b6cea01c4d63c44c6fcf143c35e` |
| `823724C8..823725B8` | `e779ddce2664d29c974858d267ff9ef3c6bc5c7281feb610645e6f0f29f7c083` |

## Device, presentation, and synchronization

| Address | Established behavior | Remaining limitation |
| --- | --- | --- |
| `0x82369420` | r3 = presentation-parameter block. Allocates 24192 bytes, aligns an internal device pointer to 128 bytes, clears 24064 bytes, calls `sub_823786A8` with device r3/parameters r4. On success returns zero and publishes device at `0x83261904`; allocation/setup failure returns `0x8007000E`. | This does not prove every device field or initialization requirement. |
| `0x8248B318` | Caller stores interval 2 at stack+164, passes block at stack+112, then calls `0x82369420` at `0x8248B480`. | Presentation interval is +52 in this edition; this alone does not decouple simulation from presentation. |
| `0x823786A8` | Device initialization calls `VdInitializeEngines` at `0x823786FC`, registers graphics interrupt callback at `0x82378740`, and checks HSIO status. | Callback timing/thread behavior still needs tracing. |
| `0x8236D688` | Ring initialization calls `VdInitializeRingBuffer` at `0x8236D8A0` and read-pointer writeback setup at `0x8236D8D8`. | Native replacement must eliminate dependent packet work or implement its required completion semantics. |
| `0x8237CA28` | Presentation path; entry r3 = device, r4 = surface/descriptor source, r5 = optional display data. Calls `VdSwap` at `0x8237CCE4`. | Complete types and all presentation branches remain unverified. |
| `0x823691D0` | Wait/progress helper; r3 points to wait state, whose +0 is device, +8 last progress value and +12 time/progress bookkeeping. Reads progress pointer at device+10896. Returns 1 to continue or 0 to stop. | Return zero is not universally proof of GPU completion: timeout/error paths also exist. |
| `0x8236D1C8` | Calls wait helper at `0x8236D284`, yielding LR `0x8236D288`. This is the existing targeted wait-backoff call. | Native fences must replace the dependency rather than force this helper to report success. |
| `0x82399BC0` | Engine counter wait invokes sleep wrapper `sub_82869478` at `0x82399BF0` with zero duration. | Original counter semantics remain authoritative; the current hook only changes the polling delay. |

`sub_8237CA28` has direct callers `sub_8248B758` at `0x8248B7B8` and
`sub_82388478` at `0x82388580`. It also calls `VdGetSystemCommandBuffer`,
`VdSetDisplayMode`, and `VdPersistDisplay`. A branch bypasses `VdSwap` when
device+21532 is nonzero. Counting entry into this routine is therefore a
presentation-path invocation count, not a proven count of displayed frames.

Shutdown path `sub_82378910` clears the interrupt callback at `0x82378AF0` and
calls `VdShutdownEngines` at `0x82378B48`. GPU callbacks must not outlive shutdown.

## Incomplete coverage and next gate

### Additional draw and binding evidence

The following entries were subsequently established from generated code and the
local `out/rhi-*` disassemblies. These are static ABI observations; runtime
frequency and full coverage remain unmeasured.

| Address | Entry ABI and established operation |
| --- | --- |
| `0x828652A0` | Non-indexed draw: r3 device, r4 Xbox primitive code, r5 start vertex, r6 vertex count. Writes the start value to GPU register `0x2102`, emits `0xC0012201` (`PM4_DRAW_INDX`) with auto-index source, and places count in the high 16 bits of the draw word. Splits large draws with topology-dependent overlap. |
| `0x82865690` | Indexed draw: r3 device, r4 primitive code, r5 start index, r6 index count. Reads index resource from device+12444 and its data pointer from resource+24. Resource word0 bit31 chooses 32-bit indices; otherwise offsets are scaled by two. Emits `0xC0032201` including index data address/extent; splits large draws. |
| `0x826D95F0` | Engine indexed wrapper: r3 device, r4 wrapper whose +8 is the index resource, r5 engine primitive enum, r6 start index, r7 primitive count. Binds through `0x82370B80`, translates enum, and converts primitive count to index count before calling `0x82865690` at `0x826D9684`. |
| `0x828648A8` | Begin non-indexed user-memory draw: r3 device, r4 primitive, r5 vertex count, r6 byte stride. Reserves count*stride bytes, builds the draw, returns writable vertex memory or zero; stores pending command endpoint at device+13444 and data pointer at +13448. |
| `0x82864D40` | User-memory draw wrapper: r3 device, r4 primitive, r5 vertex count, r6 source bytes, r7 byte stride. Calls begin helper, copies count*stride bytes, then commits the pending endpoint to device+48. Splash calls it at `0x8248CB30` with primitive 13, four vertices, stride 16. |
| `0x82864D88` | Begin indexed user-memory draw: r3 device, r4 primitive, r5 value written to register `0x2102`, r6 vertex count, r7 index count, r8 index format bits, r9 vertex stride, r10 output-index-data pointer, caller stack+84 output-vertex-data pointer. Allocates separate vertex/index uploads; returns zero on success, `0x8007000E` on failure. The meaning of the register-`0x2102` argument needs caller validation before assigning a public API name. |
| `0x82370B80` | Index binding: r3 device, r4 index resource or zero; stores device+12444 and tracks retirement of the previous resource. |
| `0x82370A68` | Vertex-stream binding helper: r3 device, r4 stream, r5 vertex buffer resource, r6 byte stride, r7 dirty mask. Reads resource data pointer +24 and size/descriptor +28; stores resource at device+12468+4*stream and stride/4 at byte device+12536+stream. No independent offset argument exists at this boundary. |
| `0x8236BD88` | Texture binding helper: r3 device, r4 sampler slot, r5 texture resource or zero, r6 dirty mask. Translates six words of texture fetch state from resource+28..+48; stores resource at device+12552+4*slot and fetch state at device+1152+24*slot. |
| `0x82370C10` | Color-target binding: r3 device, r4 target slot, r5 surface or zero. Stores target at device+12448+4*slot, copies surface descriptor fields and updates dirty/render-path state. |
| `0x82370F60` | Depth-target binding: r3 device, r4 surface or zero. Stores device+12464, copies surface descriptors into depth state, updates dirty/render-path state. |

The ordinary indexed and non-indexed draws flush dirty register banks before
packet emission. Float-constant banks begin at device+1920 and device+6016;
dirty words are at device+0 and device+8. Other dirty-state words at +16, +24,
and +32 cover additional state. Native draw hooks need a snapshot of these
states before the original flush clears their masks.

Resource-creation anchors with narrower verified scope:

- `sub_8236B998` allocates a 52-byte texture resource and backing allocations,
  initializes the header through `sub_8236B0F8`, then inserts base/mip allocation
  addresses into resource+32/+48. Input r3/r4 are dimensions, r5 depth, r6 mip
  count, r7 allocation/usage flags, r8 packed format, and r10 texture-kind code;
  callers must be checked before treating every argument as a standard D3D API.
  Returns a resource pointer or zero. The startup caller uses depth=1, mips=1,
  flags=0, kind=3 and calls at `0x8248B4F0`.
- `sub_8236BAE8` allocates a 48-byte surface resource, initializes through
  `sub_8236B3F0`, and allocates an EDRAM range when no caller description is
  supplied. Inputs r3/r4 are dimensions, r5 format, r6 sample mode, r7 optional
  allocation description. Returns resource or zero. The startup caller at
  `0x8248B4BC` passes a description; the resource is then bound as color target 0.
- `sub_823727D8` initializes a vertex declaration from a terminated element
  array. Splash writes its resulting resource directly to device+11824 and
  sets the vertex-state dirty flag. This is a confirmed inline setter path.
- Resource refcount increment `sub_823686E0` and decrement `sub_82368758` are
  investigation anchors; complete destruction/ownership semantics remain to be
  mapped. Ordinary vertex/index-buffer allocation and lock/unlock APIs remain
  unresolved. Do not mistake a physical data pointer for resource identity.

`out/mc2-rhi-call-targets.json` is a call inventory, not a D3D symbol table:
`sub_8238D410` manipulates a dynamic array, `sub_8238E5B0` serializes text, and
`sub_8238E490` formats text. Their presence as RHI callees does not make them GPU
entry points.

### Runtime shader observation

Creation-only capture reached gameplay but recorded only seven boot containers
in the initial run, so those hooks do not cover the cooked gameplay shaders.
The diagnostic also observes the verified bind setters and reconstructs a
contiguous blob from resource virtual bytes and the separate physical pointer.
The CSV marks these rows `binding_reconstructed`, preserving original creation
rows as `creation_contiguous`. Reconstructed data may contain runtime changes;
it must not be described as a byte-identical original asset without comparison.

Repeated bindings use a bounded in-memory content cache and do not read or flush
files per draw. Events record address/content changes, not complete allocation
generations; an address reused for identical shader bytes is not distinguished.

The initial binding implementation conservatively rejected addresses at or above
`0xE0000000`. Live read-only inspection identified splash shader data pointers
`0xFF5F6000` (vertex) and `0xFF5F5000` (pixel). On Windows, these are guest virtual
physical aliases with the SDK's additional 4 KiB host offset. Reading
`virtual_membase + pointer + 0x1000` exactly matched the original captured payloads
for both shaders; neither unadjusted virtual reads nor masked physical-base reads
matched. Capture now uses `rex::memory::GuestPtr`, the SDK translation helper,
for all guest reads. Bounds still reject 32-bit wrap and ranges crossing the
`0xE0000000` translation discontinuity. There is no arbitrary address mask.

The hook test reserves a full guest address span but commits only low fixture
memory and one high alias page. It exercises real high-address reads through the
capture hooks and checks that both low and high memory remain unchanged.

Verification: the rebuilt 56-case wrapper test passed. A fresh 30-second boot
(`out/boot-20261116T080053Z`) exited cleanly without shader-capture warnings;
high-alias bindings reconstructed successfully and matched original creation
hashes. Runtime-modified content remained separate rather than being discarded
as a duplicate. This establishes capture correctness for those observations,
not native shader translation or complete gameplay coverage.

Binding capture recovered 187 containers in the observed gameplay run. The final
build passed 56 context/memory passthrough cases and a fresh boot with no capture
warnings; high-address reconstructed bindings matched original creation hashes.
This validates the capture path, not native rendering or complete shader coverage.
The following remain research tasks, and block replacing the complete GPU path:

- Complete draw coverage, vertex/index buffers, texture locking/destruction, clears,
  copies/resolves, render-target selection, query/fence completion, and inline
  state writes have not been mapped comprehensively.
- Several functions emit the `0xC0003600` draw packet, including
  `sub_823688D0`, `sub_82368A28`, `sub_823716C8`, `sub_82381F68`, and
  `sub_823827D8`. Packet presence alone does not identify a public draw API;
  some of these build startup/internal draw sequences.
- Tiling paths previously investigated at `sub_8249FA80`, `sub_824A0BA8`, and
  `sub_824A1DB8` need per-pass tracing before converting to native attachments.
  Do not disable tiling or shadows as a substitute for equivalent rendering.
- Static creators may not cover shaders initialized through other routes.
  Compare runtime capture hashes against shaders seen by the working GPU
  backend before declaring coverage complete.
- Capture real containers, validate metadata/instruction support, and correlate
  shader bindings with render passes before writing the native courtyard path.
