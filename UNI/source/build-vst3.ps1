param(
 [Parameter(Mandatory=$true)][string]$LlvmMingwRoot,
 [Parameter(Mandatory=$true)][string]$WrapperRoot,
 [Parameter(Mandatory=$true)][string]$Vst3SdkRoot,
 [Parameter(Mandatory=$true)][string]$ClapSdkRoot,
 [string]$CMakePath='cmake',
 [string]$NinjaPath='ninja',
 [string]$BuildDirectory
)
$ErrorActionPreference='Stop'
$taskPackage=Split-Path -Parent $PSScriptRoot
if(-not $BuildDirectory){$BuildDirectory=Join-Path $taskPackage 'work\vst3-build'}
$taskBuild=[IO.Path]::GetFullPath($BuildDirectory)
$taskCompilerRoot=[IO.Path]::GetFullPath($LlvmMingwRoot)
$taskWrapper=[IO.Path]::GetFullPath($WrapperRoot)
$taskVstSdk=[IO.Path]::GetFullPath($Vst3SdkRoot)
$taskClapSdk=[IO.Path]::GetFullPath($ClapSdkRoot)
$taskCMake=(Get-Command $CMakePath -ErrorAction Stop).Source
$taskNinja=(Get-Command $NinjaPath -ErrorAction Stop).Source
if(-not (Test-Path -LiteralPath (Join-Path $taskCompilerRoot 'bin\x86_64-w64-mingw32-clang++.exe'))){throw 'LLVM-MinGW x64 compiler not found'}
$taskParameterHeader=Get-Content -Raw -LiteralPath (Join-Path $taskWrapper 'src\detail\vst3\parameter.h')
$taskProcessor=Get-Content -Raw -LiteralPath (Join-Path $taskWrapper 'src\wrapasvst3.cpp')
$taskTimingHeader=Get-Content -Raw -LiteralPath (Join-Path $taskWrapper 'src\wrapasvst3.h')
$taskTimingAdapter=Get-Content -Raw -LiteralPath (Join-Path $taskWrapper 'src\detail\vst3\process.cpp')
if(-not $taskParameterHeader.Contains('toPlain(Steinberg::Vst::ParamValue v)') -or -not $taskProcessor.Contains('SPATIALINK_LIMIT_SAMPLE_RATE') -or -not $taskTimingHeader.Contains('.needProjectTimeMusic()') -or -not $taskTimingAdapter.Contains('_processData.transport = _vstdata->processContext ? &_transport : nullptr;')){
 throw 'Apply source/clap-wrapper-spatialink.patch to the pinned wrapper first. See BUILD.md.'
}
& $taskCMake -S $PSScriptRoot -B $taskBuild -G Ninja `
 ('-DCMAKE_TOOLCHAIN_FILE='+((Join-Path $PSScriptRoot 'llvm-mingw-toolchain.cmake').Replace('\','/'))) `
 ('-DSPATIALINK_LLVM_ROOT='+$taskCompilerRoot.Replace('\','/')) `
 ('-DCMAKE_MAKE_PROGRAM='+$taskNinja.Replace('\','/')) `
 -DCMAKE_BUILD_TYPE=Release `
 ('-DCLAP_WRAPPER_SOURCE_DIR='+$taskWrapper.Replace('\','/')) `
 ('-DVST3_SDK_ROOT='+$taskVstSdk.Replace('\','/')) `
 ('-DCLAP_SDK_ROOT='+$taskClapSdk.Replace('\','/')) `
 ('-DSPATIALINK_OUTPUT_DIR='+$taskPackage.Replace('\','/'))
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
& $taskCMake --build $taskBuild --target UNI --parallel 4
if($LASTEXITCODE -ne 0){throw 'VST3 compilation failed'}
Write-Output ('Built: '+(Join-Path $taskPackage 'UNI.vst3'))
