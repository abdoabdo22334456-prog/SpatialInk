param(
 [Parameter(Mandatory=$true)][string]$LlvmMingwRoot,
 [Parameter(Mandatory=$true)][string]$ScratchDirectory,
 [string]$OutputPath,
 [switch]$RunTests
)
$ErrorActionPreference='Stop'
$taskPackage=Split-Path -Parent $PSScriptRoot
if(-not $OutputPath){$OutputPath=Join-Path $taskPackage 'UNI-VST3-Setup.exe'}
$taskOutput=[System.IO.Path]::GetFullPath($OutputPath)
$taskScratch=[System.IO.Path]::GetFullPath($ScratchDirectory)
$taskCompiler=Join-Path $LlvmMingwRoot 'bin\clang.exe'
$taskResources=Join-Path $LlvmMingwRoot 'bin\llvm-windres.exe'
$taskPayload=Join-Path $taskPackage 'UNI.vst3\Contents\x86_64-win\UNI.vst3'
$taskPinned=[regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'installer.c')),'expected_sha256\[\]="([a-f0-9]{64})"').Groups[1].Value
if($taskPinned -eq ('0'*64)){throw 'The final tested VST3 payload has not been pinned yet. Set expected_sha256 in installer.c to its SHA-256 before packaging.'}
$taskHash=(Get-FileHash -LiteralPath $taskPayload -Algorithm SHA256).Hash.ToLowerInvariant()
if($taskHash -ne $taskPinned){throw 'Plugin checksum differs from the pinned installer payload. Update expected_sha256 in installer.c when intentionally packaging a new plugin build.'}
New-Item -ItemType Directory -Force -Path $taskScratch | Out-Null
$taskObject=Join-Path $taskScratch 'installer-res.o'
Push-Location $PSScriptRoot
try {
 & $taskResources --target pe-x86-64 --input installer.rc --output-format coff --output $taskObject
 if($LASTEXITCODE -ne 0){throw 'Resource compilation failed'}
 & $taskCompiler --target=x86_64-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -municode -mwindows installer.c $taskObject -o $taskOutput -lcomctl32 -lshell32 -lbcrypt -luser32
 if($LASTEXITCODE -ne 0){throw 'Installer compilation failed'}
} finally {Pop-Location}
if($RunTests){
 $taskTestResource=Join-Path $taskScratch 'installer-test.rc'
 $taskTestObject=Join-Path $taskScratch 'installer-test-res.o'
 $taskTestExe=Join-Path $taskScratch 'installer-test.exe'
 $taskEscapedPayload=$taskPayload.Replace('\','/')
 [IO.File]::WriteAllText($taskTestResource,('101 RCDATA "'+$taskEscapedPayload+'"'+[Environment]::NewLine))
 & $taskResources --target pe-x86-64 --input $taskTestResource --output-format coff --output $taskTestObject
 if($LASTEXITCODE -ne 0){throw 'Test resource compilation failed'}
 & $taskCompiler --target=x86_64-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -DINSTALLER_TEST (Join-Path $PSScriptRoot 'installer.c') $taskTestObject -o $taskTestExe -lcomctl32 -lshell32 -lbcrypt -luser32
 if($LASTEXITCODE -ne 0){throw 'Installer test compilation failed'}
 & $taskTestExe (Join-Path $taskScratch 'scratch-VST3')
 if($LASTEXITCODE -ne 0){throw 'Installer scratch tests failed'}
}
Write-Output "Built: $taskOutput"
