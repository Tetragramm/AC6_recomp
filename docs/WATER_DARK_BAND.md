# The dark band across the bottom of open water

Status: **fixed** (2026-09-22) by `ac6_fix_water_bottom_band`, on by default.

## The symptom

Over open sea, a band across the bottom ~20 of 720 guest rows was near-black
and had no sparkles. Its top edge was jagged and followed the waves, it was the
same row across the full width, and it looked the same at every resolution
scale. It came and went between sessions and missions: the same binary and
config could show it in one run and not in the next.

## The cause

The sea pixel shader (`35A80CB48A481624`) multiplies its colour by a
screen-space mask, fetch constant 14 (tf14). tf14 is a 320x360 `k_8_8_8_8`
target that the game renders and resolves each frame; the water reads its B
channel (swizzle `60A`).

- The shader samples tf14 at the pixel's screen position plus a wave offset:
  ucode 26-31 compute `clip.xy / w * (0.5, -0.5) + 0.5` from interpolator 3,
  and ucode 36 adds the tf11 wave normal * 0.0312. Near the bottom of the
  screen v passes 1.0; clamp-to-edge then reads tf14's last row.
- **tf14's last 8 rows (352-359) are never written in that frame.** 360 rows
  are 22.5 EDRAM tile rows, and the game's clears and draws for this target
  only fill whole tile rows. Those 8 rows keep whatever an earlier pass left
  at that EDRAM address.
- On the host-render-target path, "whatever was left there" is whatever that
  host image last held, which depends on the passes run since start-up (menus,
  hangar, earlier missions). When it is zero, the water in the bottom band is
  multiplied by 0.

A resolve read back and dumped to an image showed it directly: rows 0-351 of
tf14 held the expected `00 FF FF FF` (an aliased depth clear at the same EDRAM
address, B = 1.0), and rows 352-359 were all zero.

## The fix

`ac6_fix_water_bottom_band` (in `spirv_translator_fetch.cpp`, AC6/Fixes, on by
default, requires a restart) clamps that one fetch - shader `35A80CB48A481624`,
fetch constant 14, 2D, normalized - to
`v <= (floor(height / 16) * 16 - 0.5) / height`, the centre of the last row
in a whole EDRAM tile row (row 351 of 360). Over open sea that row reads the
same as the rows above it, so the bottom of the screen matches the water just
above. No other shader or fetch is affected.

Verified by an A/B pair with the same binary and config: fix on, no band and
sparkles to the bottom edge; fix off, band.

The D3D12/DXBC translator does not carry the clamp. The same leftover-EDRAM
exposure exists there in principle; add the clamp to
`dxbc_translator_fetch.cpp` if it is ever seen on Windows.

## What looked guilty and was not

Each of these was checked by measurement and cleared. The first two were
wrong readings that cost time.

| Lead | What settled it |
|---|---|
| "u == v in the wave UVs" | A probe artefact: the post chain made every pixel of the probe image grey (R = G = B). |
| "The tf14 fetch is dead" | Misread destination swizzle: `tfetch2D r0._x__` writes **r0.y**, which is live. Read `rN._x__` as "which source lands in which slot", as for vfetch. |
| A patch boundary between water draws | Each water draw painted a flat colour: flat to the last row. |
| Resolve-into-texture | Toggling it for tf14 alone: no change. |
| Clear elision (colour and depth) | Toggling both, with tf14 dumped each time: rows 352-359 zero either way. |
| The mask pass (`A2F9FAB38AEABD78`, `oC0 = c216`) | Its five small meshes never reach the bottom 3%; restart indices handled correctly. |
| An out-of-range vertex fetch | The disassembly's `vf0` in that shader is really fetch constant 95, a valid buffer. |
| Depth clamp / near-plane clipping | Both backends derive them identically. |
| `profiling` / `gpu_timestamps` | Band shown with the plain player config too - it is history-dependent, not config-dependent. |

## Lessons

- **Dump the texture.** Hours of inference from probes that went through the
  post chain were settled by one read-back of the resolved image.
- **A shader probe is skipped for a shader that uses no registers**
  (`var_main_registers_` is empty), so a "make it white" probe of a
  constant-colour shader silently does nothing.
- **A bug that depends on EDRAM history looks config-dependent.** Before
  concluding a setting matters, repeat the "clean" run.

The diagnostic tooling used (per-draw colours, resolve dumps, ownership and
clear logging, per-address and per-clear A/B toggles) was removed after the
fix. It is kept as `debug/water-band-diagnostics-2026-09-22.patch` next to the
repository.
