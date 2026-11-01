param(
 [Parameter(Mandatory=$true)][string]$LlvmMingwRoot,
 [Parameter(Mandatory=$true)][string]$Vst3SdkRoot,
 [string]$PluginPath,
 [string]$ScratchDirectory,
 [string]$LegacyPluginPath
)
$ErrorActionPreference='Stop'
$taskPackage=Split-Path -Parent $PSScriptRoot
if(-not $PluginPath){$PluginPath=Join-Path $taskPackage 'UNI.vst3'}
if(-not $ScratchDirectory){$ScratchDirectory=Join-Path ([IO.Path]::GetTempPath()) 'SpatialInk-VST3-integration-tests'}
$taskCompiler=Join-Path $LlvmMingwRoot 'bin\clang++.exe'
$taskSdk=[IO.Path]::GetFullPath($Vst3SdkRoot)
$taskPlugin=[IO.Path]::GetFullPath($PluginPath)
$taskScratch=[IO.Path]::GetFullPath($ScratchDirectory)
$taskSource=Join-Path $PSScriptRoot 'test_vst3.cpp'
$taskSdkSources=@(
 (Join-Path $taskSdk 'pluginterfaces\base\funknown.cpp'),
 (Join-Path $taskSdk 'pluginterfaces\base\coreiids.cpp'),
 (Join-Path $taskSdk 'public.sdk\source\vst\vstinitiids.cpp'),
 (Join-Path $taskSdk 'public.sdk\source\common\commoniids.cpp')
)
foreach($taskRequired in @($taskCompiler,$taskSource,$taskPlugin)+$taskSdkSources){
 if(-not (Test-Path -LiteralPath $taskRequired)){throw "Missing test dependency: $taskRequired"}
}
New-Item -ItemType Directory -Force -Path $taskScratch | Out-Null
$taskExecutable=Join-Path $taskScratch 'test_vst3.exe'
# Static compiler runtime linkage keeps the test host independent of the toolchain's DLL folder.
# The two warning suppressions cover vendored SDK packing pragmas and Windows GUID printf formats.
& $taskCompiler --target=x86_64-w64-windows-gnu -std=c++17 -O2 -static -Wall -Wextra -Wno-unused-parameter -Wno-pragma-pack -Wno-format -I $taskSdk $taskSource @taskSdkSources -o $taskExecutable -luser32 -lgdi32 -lole32
if($LASTEXITCODE -ne 0){throw "VST3 test host compilation failed: $LASTEXITCODE"}
$taskScreenshot=Join-Path $taskScratch 'Interface-VST3.bmp'
$taskLog=Join-Path $taskScratch 'TEST-VST3-HOST-LOG.txt'
if($LegacyPluginPath){
 $taskLegacy=[IO.Path]::GetFullPath($LegacyPluginPath)
 & $taskExecutable $taskPlugin $taskScreenshot $taskLegacy 2>&1 | Tee-Object -FilePath $taskLog
} else {
 & $taskExecutable $taskPlugin $taskScreenshot 2>&1 | Tee-Object -FilePath $taskLog
}
if($LASTEXITCODE -ne 0){throw "VST3 integration test failed: $LASTEXITCODE; log: $taskLog"}
Write-Output "Verified actual VST3: $taskPlugin"
Write-Output "Log: $taskLog"
Write-Output "Native GUI snapshot: $taskScreenshot"
