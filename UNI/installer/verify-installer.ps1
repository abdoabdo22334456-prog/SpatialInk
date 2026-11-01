param(
 [string]$InstallerPath,
 [string]$PayloadPath,
 [string]$ExpectedVersion='0.8.0'
)
$ErrorActionPreference='Stop'
$taskPackage=Split-Path -Parent $PSScriptRoot
if(-not $InstallerPath){$InstallerPath=Join-Path $taskPackage 'UNI-VST3-Setup.exe'}
if(-not $PayloadPath){$PayloadPath=Join-Path $taskPackage 'UNI.vst3\Contents\x86_64-win\UNI.vst3'}
$taskInstaller=[IO.Path]::GetFullPath($InstallerPath)
$taskPayload=[IO.Path]::GetFullPath($PayloadPath)
if(-not ('UniVst3InstallerResourceReader' -as [type])){
 Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UniVst3InstallerResourceReader {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr LoadLibraryExW(string path,IntPtr reserved,uint flags);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr FindResourceW(IntPtr module,IntPtr name,IntPtr type);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr LoadResource(IntPtr module,IntPtr resource);
 [DllImport("kernel32.dll")] public static extern uint SizeofResource(IntPtr module,IntPtr resource);
 [DllImport("kernel32.dll")] public static extern IntPtr LockResource(IntPtr resource);
 [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr module);
 public static byte[] Read(IntPtr module,int id,int type) {
  var resource=FindResourceW(module,new IntPtr(id),new IntPtr(type));
  if(resource==IntPtr.Zero) throw new Exception("Missing resource " + id + "/" + type);
  var loaded=LoadResource(module,resource);var pointer=LockResource(loaded);
  if(loaded==IntPtr.Zero||pointer==IntPtr.Zero) throw new Exception("Cannot read resource");
  var bytes=new byte[SizeofResource(module,resource)];Marshal.Copy(pointer,bytes,0,bytes.Length);return bytes;
 }
}
'@
}
# Resource-only loading reads the EXE; it does not launch it or request elevation.
$taskModule=[UniVst3InstallerResourceReader]::LoadLibraryExW($taskInstaller,[IntPtr]::Zero,0x22)
if($taskModule -eq [IntPtr]::Zero){throw 'Cannot read installer resources'}
try {
 $taskEmbedded=[UniVst3InstallerResourceReader]::Read($taskModule,101,10)
 $taskHasher=[Security.Cryptography.SHA256]::Create()
 try {$taskEmbeddedHash=([BitConverter]::ToString($taskHasher.ComputeHash($taskEmbedded))).Replace('-','').ToLowerInvariant()} finally {$taskHasher.Dispose()}
 $taskExpectedHash=(Get-FileHash -LiteralPath $taskPayload -Algorithm SHA256).Hash.ToLowerInvariant()
 if($taskEmbeddedHash -ne $taskExpectedHash){throw 'Installer payload differs from the tested VST3'}
 $taskManifest=[Text.Encoding]::UTF8.GetString([UniVst3InstallerResourceReader]::Read($taskModule,1,24))
 [xml]$taskXml=$taskManifest
 $taskElevation=$taskXml.SelectSingleNode("//*[local-name()='requestedExecutionLevel']")
 if($taskElevation.level -ne 'requireAdministrator' -or $taskElevation.uiAccess -ne 'false'){throw 'Incorrect administrator manifest'}
 if(-not $taskManifest.Contains('Microsoft.Windows.Common-Controls')){throw 'Missing native dialog dependency'}
 if((Get-Item -LiteralPath $taskInstaller).VersionInfo.ProductVersion -ne $ExpectedVersion){throw 'Installer version metadata mismatch'}
 Write-Output 'PASS shipping installer payload SHA-256, requireAdministrator manifest, native dialog dependency and product version.'
} finally { [void][UniVst3InstallerResourceReader]::FreeLibrary($taskModule) }
