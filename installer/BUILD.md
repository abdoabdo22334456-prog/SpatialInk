# Installer source

`SpatialInk-Setup.exe` is a native Windows x64 installer containing the verified CLAP plugin as an embedded resource. Its manifest requests administrator rights because FL Studio's CLAP folder is under Common Program Files.

At launch it validates the embedded SHA-256, asks the user to install, resolves the 64-bit Common Program Files folder using Windows, and writes only `CLAP\SpatialInk.clap`. It flushes a new temporary file before atomically replacing the target. If the existing plugin is locked, replacement fails while preserving the old file. It does not download files, edit the registry, create startup entries or run FL Studio.

Build using the same LLVM-MinGW toolchain described in `../source/BUILD.md`:

```powershell
.\build-installer.ps1 -LlvmMingwRoot 'D:\Tools\llvm-mingw-20261006-ucrt-x86_64' -ScratchDirectory 'D:\Build\installer' -OutputPath 'D:\Build\SpatialInk-Setup.exe'
```

The plugin checksum is pinned in `installer.c`. When packaging a newly rebuilt plugin, explicitly update that checksum to match the intended binary. The build script checks this before compiling.

The `INSTALLER_TEST` compile-time build runs the same checksum and file-installation routines against a supplied scratch directory. Give it an RCDATA resource object containing resource 101 and **no administrator manifest**, compile `installer.c` with `-DINSTALLER_TEST`, and pass the scratch path as its only argument. It checks fresh/repeated installations, byte identity, locked-file preservation, corrupt-payload rejection, and cleanup of temporary files. The shipping executable has no target override or unattended installation option.

Remove the plugin by closing hosts using it and deleting `SpatialInk.clap` from the CLAP folder. No installer-specific files or registry entries need to be removed.
