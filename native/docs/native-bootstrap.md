# Device startup and completion dependencies

These findings apply to the executable identified in [graphics-map.md](graphics-map.md).
They come from its decoded instructions and generated functions. They identify
the next replacement boundary; a native device bootstrap is not implemented yet.
The existing native draw checks leave the working graphics system in charge.

## Host integration

ReXGlue v0.10.1 allows `RuntimeConfig::graphics` to provide an `IGraphicsSystem`.
`ReXApp::OnPreSetup` runs before the optional GPU plugin is loaded. A non-null
graphics implementation prevents the Xenos plugin from being loaded. Custom
graphics systems may leave `provider()` and `presenter()` null; the app exposes
its window's native handle and a detached overlay hook. A native renderer must
own actual presentation, resizing and synchronization in that mode.

An empty graphics configuration alone is insufficient: the original game's
device startup still builds a command ring, registers GPU callbacks, and waits
for progress. Those dependencies must be replaced together with native draws.

## Original bootstrap

`sub_82369420` allocates 24192 bytes, aligns the device to 128 bytes and clears
24064 bytes. It also allocates a 4800-byte block at device `+16728`, initializes
bookkeeping and invokes `sub_823786A8`. The global device is published at
`0x83261904` only after successful initialization.

| Routine | Observed role and remaining boundary |
| --- | --- |
| `0x823786A8` | Initializes critical sections, engines, interrupt callback and per-process device pointer; orchestrates the routines below. It cannot be retained unchanged without the original GPU. |
| `0x8236D688` | Allocates/configures primary and indirect command buffers, a 96-byte progress block at device `+10896`, and a 32-byte auxiliary block at `+10900`; initializes ring read-pointer writeback. |
| `0x823692A8` | Allocates internal GPU command data at device `+23988` and a 512-byte backing allocation at `+23996`; builds internal packets. This is not simply a general-purpose heap initializer. |
| `0x82388AA0` | Allocates storage and starts a thread, then sets its priority. The worker's complete role/lifetime still needs mapping before replacing it. |
| `0x823716C8` | Builds another internal GPU command sequence. Its effects need classification before omission or replacement. |
| `0x82387188` | Initializes cached state and dirty masks, then emits initial register/command packets. Its CPU-state work and packet emission are distinct responsibilities. |
| `0x82378250` | Reads display mode, creates texture/color/depth descriptors, binds attachments and configures presentation state. |
| `0x82377FE0` | Installs and invokes default render/sampler setters, resets texture bindings and the vertex declaration, and updates cached state. |
| `0x82384A80` | Initializes additional device bookkeeping without direct calls; individual fields remain to be classified. |
| `0x82387370` | Coordinates EDRAM retraining under critical sections. It is a hardware-specific path. |
| `0x8236DEF0` | Performs command flush/coordination through other ring helpers. It must not be treated as a host present call. |

The default-state routine iterates 101 render-state entries at `0x832219E8`
(12 bytes each) and 20 sampler-state entries at `0x83221EA8`. Device function
tables start at `+64` and `+468` respectively. Sampler defaults are applied to
26 texture slots. `sub_82387188` separately initializes the 18 vertex-fetch
descriptors following those texture slots. These are ABI/cache structures,
not a requirement to emulate the original GPU's execution.

## Completion and resource retirement

The progress pointer at device `+10896` points to 96 bytes. Its word `+0` is the
completed marker used by waits; `+4` records a command-position value. Ring
read-pointer writeback targets block `+60`. The current marker is device
`+10908`; initialization writes `current - 2` into progress `+0`.

`sub_8236CD80` emits two `EVENT_WRITE_SHD` packets (`0xC0025800`) for the command
position and marker, then advances device `+10908` by two. Its special CPU
writeback branch is guarded by other device state; it does not establish that
ordinary queued native work can be declared complete early.

The SDK reference command processor handles `EVENT_WRITE_SHD` by writing guest
memory when it decodes the packet. That guest marker is not itself a measured
host GPU completion timestamp; the reference renderer has separate host resource
lifetime handling. Native retirement must be tied to its own actual queue fences.

`sub_8236D1C8` takes device in `r3`, target marker in `r4`, diagnostic/wait reason
in `r5`, and a byte flag in `r6`. Its completion comparison is equivalent to:

```cpp
uint32_t(current - target) >= uint32_t(current - completed)
```

The unsigned differences are deliberate: marker wrap must be preserved. It may
flush when waiting for the current marker, and otherwise polls through
`sub_823691D0`. That helper can stop for timeout/recovery conditions as well as
progress; returning zero is not a substitute for a completed native fence.

Resource refcount increment/decrement (`0x823686E0` / `0x82368758`) atomically
modify resource `+4`. Type-4 resources with the relevant ownership flag recurse
through resource `+24`. Final release calls `sub_82367FA8`, which dispatches by
resource type and, for several backing allocations, waits on the resource's
last-use marker at `+8` before freeing memory. A native implementation must
retain uploads, descriptors and surfaces until the corresponding real GPU work
has completed; clearing refcounts or always passing the old wait is unsafe.

The capture build records `gpu-progress.bin` and `gpu-auxiliary.bin` alongside
the device snapshot. These are observations, not an atomic CPU/GPU snapshot or
permission to write the original renderer's progress fields from the native
comparison path.

## Implementation boundary

The next live backend needs a persistent host device/queue, resource ownership,
native presentation and real completion fences. Replacing the bootstrap must
also replace the command submission, marker publication and resource retirement
paths. Unsupported draws, copies, resolves or queries must remain explicit
failures during validation. The bounded native movie check currently proves
only that one draw family can consume live inputs correctly; it does not satisfy
this complete device contract.
