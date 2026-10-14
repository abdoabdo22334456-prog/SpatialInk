# Building Spatial Ink

The prebuilt plugin is a Windows x64 DLL with a `.clap` extension. A compiler is only needed to modify/rebuild it.

Use the portable **LLVM-MinGW** UCRT x86_64 toolchain. This release was built with `llvm-mingw-20261006-ucrt-x86_64`, downloaded from the upstream [LLVM-MinGW releases](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006). The downloaded archive SHA-256 was verified against its GitHub release digest:

`317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a`

Example from PowerShell:

```powershell
.\build.ps1 -LlvmMingwRoot 'D:\Tools\llvm-mingw-20261006-ucrt-x86_64'
```

Close any host instance using this plugin before replacing the binary. No network access is used by the build script or by the plugin.

CLAP headers are vendored from [free-audio/clap](https://github.com/free-audio/clap/tree/a47f6badb49d948fd009998f28309cdab78979c9), commit `a47f6badb49d948fd009998f28309cdab78979c9`. Their MIT license is preserved in `CLAP-LICENSE.txt`.

`test_host.c` runs integration tests against the actual compiled plugin. Build it with the same compiler:

```powershell
& 'D:\Tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang.exe' --target=x86_64-w64-windows-gnu -std=c11 -O2 test_host.c -I include -o test_host.exe -luser32 -lgdi32 -lm
```

The test harness expects a working directory containing `work/spatial-ink/` and `outputs/SpatialInk/` for its screenshot and audio outputs. To avoid writing into an existing deliverable, run it in a scratch directory with those two subdirectories created, then pass the absolute plugin path:

```powershell
New-Item -ItemType Directory -Force work/spatial-ink, outputs/SpatialInk | Out-Null
& 'D:\Build\test_host.exe' 'D:\Build\SpatialInk.clap'
```

For third-party validation, use the official [CLAP Validator](https://github.com/free-audio/clap-validator):

```powershell
clap-validator.exe validate 'D:\Build\SpatialInk.clap'
```

DSP uses lock-free 32-bit atomic parameters and bounded path publication. Delay memory is allocated during activation; audio processing performs no dynamic allocation, GUI calls or host-state calls. The GUI and saved state use a stable point/parameter representation. Processing supports in-place stereo buffers, 32/64-bit samples, sample-offset parameter/transport events and fractional sample rates between 1 and 768 kHz.

This is an analytic headphone spatial effect, not a measured HRTF renderer. See the Arabic user guide for the sonic limitations and stereo downmix behavior.
