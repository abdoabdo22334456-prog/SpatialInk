# Spatial Ink Orbit 0.3.0 — verification

Date: 26 October 2026. Compiler: LLVM-MinGW 20261006 ucrt x86_64 (archive SHA-256 verified against BUILD.md). Build: no compiler warnings with `-Wall -Wextra`.

## Integration host (`source/test_host_orbit.c`) — all passed

- Every check of the original 0.1.0 host test still passes on the new binary: GUI, 32/64-bit audio, bypass, beat sync, direction/distance cues, freehand + Undo, state roundtrip, sample-offset events, reverb tail, demo render.
- 12 host parameters (was 10).
- New ORBIT SIZE checks:
  - ORBIT button turns the live orbit on.
  - Slider drag to the right edge gives 95%, to the left edge 5%; the orbit stays on.
  - Widening ORBIT SIZE from 10% to 90% while processing makes the source quieter by **21.7 dB** (REACH 30 m, ambience off).
  - Saved orbit points have the slider radius; orbit state loads into a second instance and re-saves byte-identical.
  - After drawing a path by hand, the orbit is off and moving the slider leaves the saved path unchanged.
- New ORBIT DIRECTION checks: defaults to clockwise; the buttons set the parameter; a quarter of the way round the source is on the right ear for clockwise and the left ear for counter-clockwise (energy ratio over 2x); saved points pass the right side first for clockwise and the left side first for counter-clockwise.
- Screenshot: `Interface-Orbit.png`.

## Not done

- **Official CLAP Validator was not run** on the new binary (the tool is not installed here; not downloaded).
- **FL Studio itself was not available**: loading, editor, automation and project reopening there are unconfirmed.
- No listening test.
