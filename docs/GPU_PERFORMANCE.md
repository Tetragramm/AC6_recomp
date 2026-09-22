# GPU performance: what was done, what was measured not to work, and what a native renderer would take

Written 2026-09-16 at the end of the performance work on `linux-port`
(merge `e66de6ca` and after); sections 5-7 added 2026-09-21 on
`perf/edram-transfer-elision` (`79d6adbc`..`9198aad4`). Everything here is
measured on an RTX 2080 Ti with the NVIDIA proprietary driver at 4K unless a
laptop is named; "scale 2" and "scale 3" mean `resolution_scale`.

## 1. Results

A heavy mission at scale 2 went from 45-55 fps to a flat 60, and the GPU frame
from 18.7 ms to about 11 ms. Scale 3 runs 13-15 ms and holds 60 except in the
worst explosions, where it dips to the mid-50s. The frame-rate drops are purely
GPU-bound: every profiler report below 60 fps has the GPU over 16.5 ms while
the command-processor thread *falls* to ~50% of a core, waiting on the fence.

The changes, in order of effect:

| change | cvar | effect |
|---|---|---|
| Release the swap-complete worker from the command processor when a memory wait would sleep (the ring was vsync-throttled by the guest's own worker) | `ac6_swap_flip_on_fence` | 45-55 -> flat 60, game clock untouched |
| A full-target D3D `Clear` takes EDRAM ownership without copying in the contents it overwrites | `ac6_clear_elides_edram_transfer` | -7 ms of GPU at scale 2 |
| A resolve writes the destination texture's `VkImage` directly - one copy (or `vkCmdResolveImage` for averaged MSAA) instead of a tiled-memory write and a compute untiling at the consuming draw | `vulkan_resolve_to_texture_image`, `_msaa` | untiling collapses; 15.5 -> 12 ms |
| A compute resolve for what the copy cannot express (depth destinations, multisampled views) | `vulkan_resolve_to_texture_compute` | untiling 1.6 -> 0.5 ms |
| Stencil in EDRAM transfers by one compute pass and a buffer copy instead of eight masked draws per sample (no `VK_EXT_shader_stencil_export` on this driver) | `vulkan_edram_stencil_transfer_compute` | 1.47 -> 1.1 ms |
| Skip carrying stencil across transfers | `ac6_edram_skip_stencil_transfers` | -1.4 ms; OFF by default, see below |
| A real CPU profiler and GPU timestamp attribution | `profiling`, `gpu_timestamps` | made all of the above measurable |

`ac6_edram_skip_stencil_transfers` is off by default because it is not proven
safe: two missions looked identical with it on, but the game enables the
stencil test on ~188 draws a frame and ~114 of them test with a real compare
function, so a scene that needs stencil to survive a reinterpretation would
break. It is a user choice, and the comment in the config says what to watch.

## 2. Where the frame goes now

Scale 3, busy scene, ~15 ms (scale 2 is roughly the same picture at ~7-12 ms):

| bucket | ms | nature |
|---|---|---|
| the game's draws (world pass at 2x MSAA, post chain) | 5.3 | inherent |
| EDRAM ownership transfers, ~15 alias patterns | 5.3 | load-bearing (section 3) |
| resolve family: copy 2.4 + its barriers 0.5 + tiled-path fallbacks 1.1 + untiling 0.5 + compute 0.4 | 4.4 | copy runs at ~300 GB/s of traffic, about half of peak |

The GPU is execution-bound, not bandwidth-bound: `nvidia-smi dmon` during the
heavy mission shows SM busy 99-100%, memory controller 24-29%, power 240-248 W
of a 250 W cap.

The emulator issues 1.7-3.1x as many draws as the game makes D3D draw calls,
because AC6 renders the world in predicated tiles (two 640-wide tiles of a
1280-wide frame) and each draw is issued once per tile.

## 3. What was tried and measured not to work

Each of these is committed on a branch or recorded in a commit message so it
is not re-tried without new information.

- **Compute resolve instead of the image copy for everything** - slower: 3.74
  vs 2.36 ms for the same resolves, draw cost matched to 0.2% between runs. Copy
  runs first; `vulkan_resolve_to_texture_prefer_compute` re-tests it.
- **`vkCmdBlitImage` instead of `vkCmdCopyImage`** - no faster (1127 vs 1057
  us).
- **Handing the render target image to the texture instead of copying**
  (alias + copy-on-write) - impossible: 100% of copied bytes fail on image
  shape. A render target image is as tall as the EDRAM addressing period
  allows; the texture is as tall as the guest texture; Vulkan has no cropping
  image view and no defined common layout for two optimally tiled images of
  different extents.
- **Async compute on a second queue** - nothing to gain: the GPU is
  continuously busy at its power cap (above). Never got code.
- **Magic-number division in the transfer shaders** (branch
  `perf/transfer-shader-alu`) - no difference. The transfer shaders are bound
  by the per-pixel `texelFetch` from a multisampled source, not ALU.
- **CPU multithreading** - the command processor thread idles *more* when
  frames drop; nothing on the critical path to parallelize.
- **The fragment-shader-interlock EDRAM path** (`render_target_path_vulkan =
  "fsi"`) removes every transfer, and renders correctly, at 27-36 ms per
  frame: it does depth test, stencil test and blending by hand under an
  interlock on every pixel, and every resolve optimization above is gated on
  the host-render-target path.
- **Eliding transfers whose destination the draw fully overwrites** (like the
  Clear) - reported here on 2026-09-16 as "zero of ~6500 transferred tiles a
  frame qualify". **That was a measurement artefact**: the check required
  `PA_CL_VTE_CNTL == 0x300` (screen-space vertices), and every
  post-processing quad uses a real viewport (vte `0x43F`), so the whole post
  chain was excluded. Judged by viewport and scissor extents, 34-51% of the
  tiles qualify. It is built and on - see section 5.
- **Skipping redundant ping-pong transfers** - none exist; ownership tracking
  already prevents a repeat without an intervening write.
- **Tile block copies for pitch-only reinterpretations** (branch
  `perf/edram-transfer-by-copy`) - correct, validation-clean, and covers 0.9%
  of transferred tiles: AC6's pitch alias also changes the colour format, so
  the host images are different types and the transfer converts values.
- **Resolving straight from the depth target instead of transferring depth into
  a colour target** - those colour targets are drawn into 5 times a frame and
  never merely resolved.

The one-line summary of the transfers: they are re-typings of the same bytes
at sample granularity or across formats, which is exactly the work a shader
has to do. They are the emulation, not overhead on top of it.

## 4. The full native renderer: what it would require

Everything above optimizes a faithful Xenos emulation. The remaining 10 ms of
the scale-3 frame that is not the game's own shading exists *because* the
emulation is faithful: EDRAM aliasing needs transfers, resolves need copies,
tiling doubles draws. A renderer that intercepts the game's D3D9 calls and
renders their *intent* natively removes all three. Estimated ceiling: ~15 ms
to ~6-7 ms at scale 3. Nothing else is within an order of magnitude of that.

This is the project the plan calls Design A (stages 2-6); stages -1, 0, 0.5,
0.75, 1 and a redirected 5 are done. What it would take:

### Ground truth already established (keep it)

- **The guest D3D device struct is a Xenos register shadow**, offsets proven
  by the register-shadow verifier: 6192 title draws with 5.25 million register
  writes and zero mismatches; a mission with 0.004% mismatches, all walker
  artefacts. `src/ac6_register_shadow.{h,cpp}`, cvar
  `ac6_register_shadow_verify`. Two facts it settled: expand the struct
  *after* the draw (helpers `821ED210`/`821EBD40` derive `SQ_*` and
  `RB_HIZCONTROL` at draw time), and `VGT_INDX_OFFSET` is a draw argument (r5).
- **The D3D9 hook map in `src/d3d_hooks.cpp` is corrected** (12 of 19 labels
  were wrong): `SetRenderTarget 821DD260`, `SetDepthStencilSurface 821DD5C8`,
  `SetVertexShader 821DE600`, `SetPixelShader 821DE308`, `SetTexture 821E10C8`,
  the window-scissor emitter `821DA698`, `BeginTiling 821E4630`, `EndTiling
  821E4AD0`, and the kill switch `821E5FD0` - the only route to the
  `CP_RB_WPTR` write.
- **The swap protocol** (scratch mailbox at `0x1F068000`, vblank ISR
  `0x821E63F0`, swap-complete worker `0x821EFBE0`) is understood well enough
  that the emulator already drives it.
- **Instrumentation**: per-site CPU profiler, GPU timestamps per pass, and the
  counters added this session (draw multiplier, transfer tiles, stencil use).

### The semantic unknowns (this is the actual work)

Run F proved the aliases are load-bearing. We know what every one **costs**;
a native renderer needs to know what each one **means**, because it must
reproduce the intent rather than the mechanism. Per pattern, in cost order:

| pattern | what must be understood |
|---|---|
| `depth 720/16/4x -> color 0/16/1x`, then drawn into, then resolved | why the game renders *into* its depth bits viewed as colour, and what its shaders unpack from the resulting texture |
| `color 0/8/1x -> color 0/16/1x` with a format change | SETTLED as far as it matters: the first draw into the 1280-wide view is a full-viewport unblended quad, so the copy is discarded - elided (section 5) |
| `depth 720/8/4x -> depth 720/8/2x` (stencil and depth) | SETTLED: the 360 D3D fast-clear packing - the Clear quad draws at 4x so each pixel clears four samples; the transfer propagates a constant |
| `color 0/5/4x -> color 0/5/1x` (~14x a frame, ping-pong) | the 200-px 4x effects target read as 400-px 1x samples-as-pixels: a custom resolve or a blur |
| the 4x-as-1x reads generally | which shaders consume them and whether they need individual samples or would accept an average |

The method that answers these is the one that worked all session: make the
shader show its work - log the resolves, the fetch constants and the shader
hashes that consume each aliased texture, and read the consuming ucode.

### The stages, restated with what is now known

1. **Native draw path, still tiled, PM4 still on** - route intercepted draws
   into `IssueDraw` with the synthesized register file, render offscreen, and
   diff per pass against the emulated output at scale 1. The snapshot rule
   still applies: the native path must be read-only on guest memory during
   dual-run, and must copy vertex/index/constant data at intercept time.
2. **Cut the ring** - no-op `821E5FD0`; stub the fence/interrupt side
   (`821E4378`, `821E44C0`, `device+13500`/`+21516`) so the guest still sees
   frames complete. First real number: PM4 emit and parse cost is zero.
3. **Collapse predicated tiling** - DONE, without the native draw path, as
   the wide host render target (section 6). The 2026-09-16 claim that "the
   tiling is in the game's own EDRAM base arithmetic" was wrong: the two
   tiles share one EDRAM key and differ only in `PA_SC_WINDOW_OFFSET` and the
   window scissor.
4. **Replace EDRAM** - render targets become native attachments, resolves
   become render-to-texture, and each alias pattern from the table above is
   reimplemented by intent. This is where the semantic work lands.
5. **Remove the guest-side work** - no-op the register flushers last, because
   while they run they are stage 1's ground truth.
6. **Then the inherited costs** - descriptor and sampler caching (the
   translator rewrites texture descriptors every draw), the deferred command
   buffer's double recording, a presenter thread, real Vulkan vertex input in
   place of SSBO fetch.

### Validation

There is no reference except the emulated output. Capture per-pass images
from the emulated path once (attract mode reaches a 3D scene unaided ~60-90 s
after boot; missions need the user at the controls) and diff each native pass
against them, gated on SSIM, using the existing pass classification. Every
alias pattern needs its own test scene, because each is used by different
effects.

### Honest estimate

Months, not weeks, for someone who already has the ground truth above. The
characteristic failure is subtle, scene-specific visual corruption - the
expensive kind to chase - and the two false alarms this session (a black
screen from a check placed after the point of no return; a "draws that write
nothing" finding that was a stale counter) are the shape of what to expect.

It is worth doing only if one of two things becomes true: scale 3 or higher
needs to be rock-solid, or the target is GPUs weak enough that 2x is the
difference between playable and not. At a flat 60 at scale 2 with headroom,
it is not.

## 5. 2026-09-21: the frame narrative, and what it found

The method the plan called for - "make the shader show its work" - became a
tool: `narrate_frame_every_s = N` writes one whole frame to the log as
`[NARR]` lines, every draw with its targets, the transfers it paid for, its
shader hashes and the textures it reads (each traced to the resolve that
produced it), every resolve with source, sample select and destination.
`debug/analyze_narrative.py` turns a frame into passes, transfers by alias
pattern with a verdict, consumers and resolves. The test configs are now
generated from the shipped `ac6recomp.toml` (`run-real.toml`) after a stale
base config produced a false "texture load is 42% of the frame" for a
morning; a cvar under test alternates on a timer so one run gives A/B blocks
of the same scene.

What one mission frame settled:

- **The world pass is one 1280x720 2x MSAA image submitted twice.** Draws
  d61-d268 and d270-d477 are 208 identical draws (shader, primitive, vertex
  count); they differ only in window offset (`0,0` vs `-640,0`) and scissor,
  share one EDRAM key, use the same 1280-wide viewport. The two 640-wide
  resolves land `0x14000` bytes apart - exactly 640 px in a 32x32-tiled
  4-byte texture - in one 1280x720 texture. The colour resolve is `avg01`:
  real antialiasing. (An earlier note that 2x samples "sit side by side"
  was wrong: `pixel_size_y` doubles at 2x, `pixel_size_x` at 4x.)
- **34-51% of transferred tiles are copied into a target whose first draw
  overwrites them completely**, unblended (2776-3496 of 6040-6820 tiles
  across five frames). Two things a naive test gets wrong: only a
  screen-filling quad may be claimed (draws with 207 and 1826 vertices pass
  every state test and cover an unknown subset), and depth is not colour
  (d482 overwrites all colour while depth-testing against the copied depth).
- **48 draws every frame write nothing** - one-vertex point lists, colour
  mask 0, no pixel shader: the game's D3D perf-counter markers.

The changes, measured on the user's real config (4K, scale 3, 60 cap):

| change | cvar (all default on) | effect |
|---|---|---|
| A full-viewport unblended quad takes EDRAM ownership without the copy, coverage judged by viewport and scissor; depth only when depth and stencil are both rewritten | `ac6_quad_elides_edram_transfer` | transfer tiles 6648 -> 4430 (-33%), transfer buckets 3.47 -> 2.00 ms, measured against the busier phase |
| Draws that rasterize but cannot write are not issued | `ac6_skip_no_effect_draws` | 48 draws/frame |
| TYPE0 PM4 packets apply consecutive registers through the range path (one copy per constant block) instead of one virtual write per register | none - semantically identical | PM4 walk 0.071 -> 0.027 ms/frame at the title, -64% |
| The world rendered once into a wide host render target | `ac6_wide_world_target` | 253 of ~890 draws/frame not issued; GPU 13.62 vs 13.75 ms; CP busy 34.5% vs ~40% |

The first version of the wide target moved the halves with image copies -
an MSAA image copy runs at ~40 GB/s on NVIDIA in any layout, ~3.5 ms per
half at scale 3, and it put the mission at 40 fps. The shipped version
copies nothing (section 6).

## 6. The wide world target

A render target keyed "640 wide, 2x MSAA" (`IsWideKey`: pitch 8 tiles at
32bpp, 2x) gets a host image twice as wide. Its EDRAM key is untouched, so
ownership, transfers, dumps and resolves see nothing new. The command
processor drops draws with the second tile's window offset and lets the
first tile's scissor span the whole width. The right half of the image is
outside EDRAM; three rules keep it right:

1. **Transfers and clears into a wide target apply to both halves.** The
   transfer rectangles are drawn a second time at +width, and the transfer
   shader takes destination x modulo the target's pitch, so a right-half
   fragment addresses the same EDRAM as its left-half twin (identity for
   every other target). Exception: a resolve-clear that arrives *between*
   the target's two tile resolves - the first tile's colour resolve clears
   depth for the second - stays on the left half, because the second tile's
   contents already sit in the right half. Doubling that clear was the
   "clouds through mountains" bug.
2. **The second tile's resolve samples the right half.** Recognised as a
   resolve of the same span with no draws into the wide target since the
   first; the dump/direct resolve shaders take a source x offset in tiles
   (`DumpPitches` bits 20-27, a `DirectResolvePushConstants` member), the
   image copy an offset in pixels. Ownership is handed back to the wide
   target first when the guest's resolve-clear through an alias key moved
   it (the hangar resolves the world through a 640x1440 1x view).
3. **If another target that has taken over the span is drawn into between
   the two resolves, the second tile renders normally.** The hangar room
   restores last frame's scene per tile - colour and depth, from a
   1280x1440 texture - through the 1x view, so its tiles genuinely start
   from different states. It falls back to the old cost, correctly.
   (Detected when it happens, in `Update`; the awaiting entries must
   survive the second tile's own rebind or the flag clears after one draw.)

Three frame shapes are known and verified by eye: the mission (2x-key
resolves), the grey assembly screen (1x-view resolves, no draws between the
tiles), the hangar room (per-tile injection). Diagnostics:
`ac6_wide_world_target_log = N` prints the state machine's decisions;
`ac6_world_drop_second_tile` is the measurement-only drop that priced the
work.

## 7. The laptop profile, and what it changes

The first profile from an affected machine (Radeon 780M, RADV, scale 1)
settled the question section 4 left open: **it is bound on the command
processor thread**, at 100% of a core, at 10-12 fps in a mission, with the
GPU at 13-17 ms - it could do ~60 - and the guest's main thread spinning in
the D3D fence wait. Every GPU-side win in this document is irrelevant to
that machine; per-draw CPU cost times draw count is the lever, which is why
the wide target was built.

That profile also lied by ~70%: functions whose real work is trivial and
identical on both machines measured 1.5-2.0 us there against 0.03-0.09 on
the desktop - the two `steady_clock` reads per scope on a machine whose
clocksource is not the TSC. The profiler now accumulates hardware ticks
(`rdtsc` / `cntvct_el0`) and converts at report time; the next laptop profile
is trustworthy. Corrected, the laptop's real per-draw cost is ~2x the
desktop's, not 8-20x. Two asks remain outstanding: a profile with the
current build, and the tester's
`/sys/devices/system/clocksource/clocksource0/current_clocksource`.

Where the native renderer stands against section 4: stage 3 is done another
way; stage 5's remaining question (what the aliases mean) turned out to
matter less than what the draws *do* to them, which the narrative answers
per frame; stages 1-2 (cut PM4) are a smaller prize than they looked - the
PM4 walk is ~3.5 ms of a 60 ms laptop frame after the range-path fix - and
stage 6 (per-draw state caching) is the next candidate, pending the
corrected profile to say which per-draw subsystem dominates.
