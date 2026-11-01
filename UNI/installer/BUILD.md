# UNI native VST3 installer

The shipping EXE embeds one self-contained Windows x64 `UNI.vst3` DLL payload. It creates the fixed modern VST3 bundle layout and installs only that DLL at `Common Files/VST3/UNI.vst3/Contents/x86_64-win/UNI.vst3`. It does not download dependencies, change registry entries, launch a DAW, or replace CLAP files.

Close audio hosts before installation. The EXE requests Windows administrator approval and shows a native installation dialog. It retains the first previous inner DLL as `UNI.vst3.pre-update.bak` next to that DLL; future installs do not overwrite this backup. Each fixed directory component is checked against file collisions and reparse points. A loose file at the root bundle name is preserved and rejected with a clear message. There are no recursive deletes or directory replacements. If an old-name `SpatialInkOrbit.vst3` file or bundle exists beside the intended UNI bundle, installation stops and preserves it. Move that old item to a backup outside all VST3 scan paths first; the rename retains the same VST3 class ID, so both versions must not stay in scan paths together.

Place the final tested folder bundle in the package parent directory. Set `expected_sha256` in `installer.c` to the inner DLL's lowercase SHA-256. The all-zero placeholder deliberately prevents packaging before a payload is finalized.

```powershell
Get-FileHash ..\UNI.vst3\Contents\x86_64-win\UNI.vst3 -Algorithm SHA256
.\build-installer.ps1 -LlvmMingwRoot 'D:\Tools\llvm-mingw-20261006-ucrt-x86_64' -ScratchDirectory 'C:\Build\UNI-VST3-installer' -RunTests
.\verify-installer.ps1
```

`-RunTests` compiles and runs a test variant without an administrator manifest. It writes only to a new uniquely named subdirectory under the requested scratch directory. Tests cover payload checksum, fixed bundle creation, first/repeated installation, a locked target, corruption rejection, temporary-file cleanup, preservation of the first backup, preservation of an unrelated CLAP file, rejecting a directory at the inner DLL path, preserving/rejecting a loose file at the root bundle name, and preserving/rejecting old-name files and bundles to prevent duplicate class IDs.

`verify-installer.ps1` reads the shipping EXE as a resource file without executing it. It compares the embedded plugin checksum to the tested payload and verifies its administrator manifest, native dialog dependency, and version metadata.

Running these tests does not perform or verify an actual administrator installation in Program Files, and does not verify loading or automation inside a DAW. Those checks must be reported separately.
