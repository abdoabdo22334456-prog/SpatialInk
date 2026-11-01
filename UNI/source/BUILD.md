# Build UNI VST3 0.8.0

The production plugin is a native Windows x64 VST3 bundle. Its C audio engine and Win32 UI are statically linked with the official CLAP-to-VST3 wrapper. An exported embedded `clap_entry` is resolved inside the same DLL; no external CLAP install, Node.js, Electron, browser, server, or compiler is required to use the result.

## Build dependencies

- Windows x64 and LLVM-MinGW UCRT x86_64. The tested toolchain is `llvm-mingw-20261006-ucrt-x86_64` (Clang 23.1.3).
- CMake 4.4.4 and Ninja 1.13.2 are the tested/recommended versions for reproducing this build.
- Git for downloading pinned sources and applying the compatibility patch.

Pinned source revisions:

| Source | Revision |
| --- | --- |
| free-audio/clap-wrapper | `806bba0a5f792e3966b99c1561ab9952d3316d7c` |
| steinbergmedia/vst3sdk | `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96` |
| free-audio/clap | `a47f6badb49d948fd009998f28309cdab78979c9` |

Download these official repositories to a separate dependency directory. In the VST3 SDK, initialize `base`, `cmake`, `pluginterfaces`, and `public.sdk` submodules. The plugin build does not need VSTGUI, samples, tutorials, or documentation submodules.

Example from the package directory (Git downloads require network access):

```powershell
git clone https://github.com/free-audio/clap-wrapper.git work/deps/clap-wrapper
git -C work/deps/clap-wrapper checkout 806bba0a5f792e3966b99c1561ab9952d3316d7c
git clone https://github.com/steinbergmedia/vst3sdk.git work/deps/vst3sdk
git -C work/deps/vst3sdk checkout 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
git -C work/deps/vst3sdk submodule update --init base cmake pluginterfaces public.sdk
git clone https://github.com/free-audio/clap.git work/deps/clap
git -C work/deps/clap checkout a47f6badb49d948fd009998f28309cdab78979c9
$patch = (Resolve-Path source/clap-wrapper-spatialink.patch).Path
git -C work/deps/clap-wrapper apply --ignore-whitespace $patch
```

The small wrapper patch records three necessary adaptations: the SDK 3.8.1 queryInterface macro uses `_iid`; VST3 plain/normalized conversions must use the declared parameter ranges; unsupported rates must be rejected before replacing a valid processing setup; the wrapper must request the host timing fields, map musical-position validity correctly and pass a null transport if no VST3 context is supplied. The last check is enabled only for this plugin by `SPATIALINK_LIMIT_SAMPLE_RATE`.

## Compile

```powershell
.\source\build-vst3.ps1 `
 -LlvmMingwRoot 'D:\Tools\llvm-mingw-20261006-ucrt-x86_64' `
 -WrapperRoot '.\work\deps\clap-wrapper' `
 -Vst3SdkRoot '.\work\deps\vst3sdk' `
 -ClapSdkRoot '.\work\deps\clap' `
 -CMakePath 'D:\Tools\cmake\bin\cmake.exe' `
 -NinjaPath 'D:\Tools\ninja\ninja.exe'
```

Result: `UNI.vst3/Contents/x86_64-win/UNI.vst3`. Copy the **entire UNI.vst3 folder** when installing manually. Windows/UCRT system DLLs are the only dynamic imports; C++ runtime support is statically linked.

## Verify and package

`test-vst3.ps1` builds and runs the included native VST3 host harness against the actual plugin. See its parameter list for SDK/compiler paths. The Steinberg SDK `validator` target can also be built and run against the bundle root. On LLVM-MinGW the validator's `wmain` requires `-municode`; the validator used here was linked with `-static -municode`.

The VST3 interface supports **32-bit floating-point audio samples**, and correctly reports 64-bit samples unsupported. This is separate from the plugin's **64-bit Windows architecture**. Supported rates are 1–768 kHz, with stereo input/output.

After any intentional rebuild, rerun tests, update `expected_sha256` in `installer/installer.c` to the inner DLL's SHA-256, then use `installer/build-installer.ps1 -LlvmMingwRoot <toolchain> -ScratchDirectory <scratch> -RunTests`. Run `installer/verify-installer.ps1` against the generated installer as described in `installer/BUILD.md`.

The packaged installer requests administrator rights to install into Common Files/VST3. Its elevated production run and FL Studio GUI behavior were not tested in this session. See `TEST-REPORT.md` for the observed validation results.

## Tempo, speed, direction, Double and Two Pages checks (0.8.0)

The engine includes `tempo_clock.h`. The VST3 integration host verifies all 39 automatable parameter lanes, two independent processing pages, separate paths and clocks, both audible outputs, optional Double Orbit, actual motion at multiple host BPM values, transport seeks/loops, missing contexts and reverse DRAW/ORBIT playback. It also compares Single and Double Orbit audio with the actual 0.6.0 and 0.7.0 binaries after state migration. The plugin requests the required VST3 host timing fields.

The small clock unit suite can also be run separately:

```powershell
New-Item -ItemType Directory -Force work/clock-tests | Out-Null
& 'D:\Tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang.exe' --target=x86_64-w64-windows-gnu -std=c11 -O2 source/test_tempo_clock.c -I source -o work/clock-tests/test_tempo_clock.exe -lm
.\work\clock-tests\test_tempo_clock.exe
```

Parameter IDs 0-19 stay stable. ID 20 is Two pages and IDs 21-38 mirror all original controls for Page B. Existing ID 4 (Path position) controls Page A, while Page B uses ID 25. In Two Pages + Follow Transport, each page's Path position is its offset from that page's own clock; in Manual it is an absolute position. Double Orbit remains a separate mode using the existing IDs 18 and 19. State format 7 reads formats 3-6, supplying Single mode and Page B defaults for old projects. Saves from 0.8.0 are not readable by older plugin builds. The VST3 class identity and all old automation IDs remain unchanged.
