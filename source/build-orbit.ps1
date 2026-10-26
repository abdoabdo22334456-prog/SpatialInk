param([Parameter(Mandatory=$true)][string]$LlvmMingwRoot)
$ErrorActionPreference = 'Stop'
$taskCompiler = Join-Path $LlvmMingwRoot 'bin\clang.exe'
if (-not (Test-Path -LiteralPath $taskCompiler)) { throw "Expected LLVM-MinGW compiler: $taskCompiler" }
$taskPackage = Split-Path -Parent $PSScriptRoot
$taskSource = Join-Path $PSScriptRoot 'spatial_ink_orbit.c'
$taskHeaders = Join-Path $PSScriptRoot 'include'
$taskOutput = Join-Path $taskPackage 'SpatialInkOrbit.clap'
& $taskCompiler --target=x86_64-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -shared $taskSource -I $taskHeaders -o $taskOutput -luser32 -lgdi32 -lm
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
Write-Output "Built: $taskOutput"
