# Orbit Size slider + renamed build (design, small change)

Date: 2026-10-26

## Intent
The ORBIT button currently draws a fixed circle (radius 0.5 on the map, `preset(p,0,…)` in `source/spatial_ink.c`). The user wants a slider that widens/narrows that orbit, and a new build under a new name that installs next to the original Spatial Ink without clashing.

## Goals
- New parameter **ORBIT SIZE**, shown as a slider in the right-hand panel with the other sliders.
- While the current path is an orbit, moving the slider regenerates the circle live, so the user hears it as they drag.
- Automatable from the host, saved and restored with the project state.
- Default value equals today's orbit size, so pressing ORBIT without touching the slider behaves as before.
- New plugin identity so FL Studio lists it as a separate plugin.

## Non-goals
- No change to DRAW, PLACE, FAR > EAR, WHISPER, CLEAR, UNDO.
- No scaling of hand-drawn paths (that was option B, rejected).
- No change to DSP, REACH, or the distance mapping.

## Anti-goals
- Must not break existing audio behavior or alter the saved path format.
- Must not overwrite or replace the original `SpatialInk.clap`.

## Approaches considered
- **A (chosen):** live automatable ORBIT SIZE parameter; regenerates the path only while the path is an orbit.
- B: slider scales any path. Rejected, it collides with REACH.
- C: slider applied only when ORBIT is pressed. Rejected, no live feedback or useful automation.

## Decisions (user's)
- Slider named for orbit size, live, option A.

## My calls (please check)
- New name: **Spatial Ink Orbit**; CLAP id `local.spatialink.orbit`; file `SpatialInkOrbit.clap`; window class `SpatialInkOrbit.CLAP.0.2`; version `0.2.0`.
- Range: about 0.05 to 0.95 of the map radius, default 0.5, shown as a percent.
- "Path is an orbit" is tracked with a flag set by the ORBIT button and cleared by any other path edit (draw, place, preset, clear, undo to a non-orbit path).
- The original `SpatialInk.clap` and its source files stay untouched; the new source is a copy (`source/spatial_ink_orbit.c`), built with a new script line.
- States saved from the old plugin are not loaded (different id).

## Left to the builder
- Parameter enum/definitions, slider layout in the panel (right panel may need slight re-spacing for a 9th slider), UI mouse hit-testing, state save/load of the new value and flag.
- Build with `build.ps1` (LLVM-MinGW) and re-run `test_host.c` plus CLAP Validator; update README-AR.md and SHA256SUMS.
