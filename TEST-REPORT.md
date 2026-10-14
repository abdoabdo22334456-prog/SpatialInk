# Spatial Ink 0.1.0 — verification

Date: 14 October 2026. Target: Windows x64, CLAP. Tested the actual compiled `SpatialInk.clap` binary, loaded through its exported CLAP entry point.

## Official CLAP Validator

[free-audio/clap-validator](https://github.com/free-audio/clap-validator/releases/tag/0.4.1), Windows release `0.4.1-127-g152b982`:

**44 tests considered: 34 passed, 0 failed, 0 warnings, 10 skipped.** Skips cover Unix-specific loading and optional features this audio effect does not expose, including note ports, preset discovery and optional audio-port configuration/activation extensions.

An earlier run executed alongside the integration host emitted one denormal-processing timing warning (about 2x relative time). The complete isolated run with one validator worker did not reproduce it. Processing explicitly enables and restores SSE flush-to-zero/denormals-are-zero flags; these timings should still be treated as local measurements.

Passing checks include descriptor/factory consistency, scan time, parameter defaults/conversions/namespace handling, random and boundary parameter fuzzing, sample-offset events, transport fuzzing (including intra-block updates), state reproducibility, invalid-state handling, in-place/out-of-place audio, 32/64-bit samples, denormals, changing sample rates and block sizes, reset/reactivation and processing-status/constant-mask behavior.

## Integration host

The included C test host checks:

- Actual DLL loading, two initialized instances and repeated entry initialization.
- All 10 host parameters and stereo input/output declarations.
- Native Win32 editor creation, embedding, DPI scaling, screenshot rendering, destruction/recreation and floating-mode creation.
- Exact dry and bypass output in both sample formats, plus activation and processing at 1, 8, 44.1, 48, 96, 192, 384 and 768 kHz.
- Source marker follows host beat position, respects 3/4 versus 4/4 meter and returns to the start on a cycle-boundary seek.
- A left source produces stronger left-channel energy: about **10.7 dB** difference in the test configuration.
- A near source versus a far source produces about **34.0 dB** total-energy difference with ambience disabled in the test configuration.
- A right source arrives in the right ear about **24 samples** before the left at 48 kHz, in the test configuration.
- Freehand drawing, equal-distance path sampling, Undo and host edit notifications.
- State streams supporting partial reads/writes; identical saved-state roundtrip; invalid-state rejection without a partial replacement; identical audio recall in two independent instances.
- Reset removes all buffered audio tails.
- Parameter events apply at the specified sample offset, including in-place audio buffers.
- A large-room impulse tail remains finite and decays; late/early energy ratio about `9.3e-13` in the measured windows.
- An 8-second stereo demo is generated through the plugin using host transport at 120 BPM. Its export took about 0.08–0.10 seconds on this machine, including file writes; this is a local smoke measurement, not a general CPU benchmark.

## Scope

The drawing plane is horizontal. Height is an additional approximate spectral control. The renderer uses analytic binaural cues rather than measured HRTFs, and its subjective front/back or elevation accuracy has not been measured in listener tests.

**FL Studio itself was not available for an end-to-end test in this environment.** The binary passes the independent CLAP checks above; loading, editor integration, automation and project reopening in the user's particular FL Studio build still need confirmation there. Image-Line documents CLAP support from FL Studio 2024.1 onward.

The plugin has no network functionality. Imported libraries are Windows USER32/GDI32/KERNEL32 and UCRT components. It does not need the compiler at runtime.

The source can be rebuilt and the integration tests rerun using the instructions in `source/BUILD.md`.

## Installer verification

`SpatialInk-Setup.exe` embeds the same verified plugin binary and a manifest requesting administrator rights for Common Program Files. The shipping executable was read as a resource-only file (without executing or elevating it); its embedded plugin SHA-256, administrator manifest, native-dialog dependency and version metadata were checked.

A separate non-elevating test build ran the production checksum and copy routines against a scratch folder. Fresh and repeated installations produced a byte-identical plugin. Updating a locked target failed and preserved the existing plugin. A corrupted payload was rejected before writing, and failed updates left no temporary installation files.

The installer itself was not run against the system CLAP folder, and FL Studio was not started by these tests. It installs only `SpatialInk.clap`; it makes no registry or startup changes and has no network functionality. Its source and build instructions are included under `installer/`.
