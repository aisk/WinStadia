# Builds the UMDF driver package into driver\out\pkg. It is left unsigned;
# install.ps1 signs it on the machine it gets installed on.
# Uses the NuGet WDK unpacked in .wdk (no system-wide WDK install needed).
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$wdk = Join-Path $root '.wdk\wdk\c'
$sdkVersion = '10.0.26100.0'
$umdfVersion = '2.31'

if (-not (Test-Path $wdk)) {
    $wdkPackage = 'microsoft.windows.wdk.x64'
    $wdkPackageVersion = '10.0.26100.6584'
    $archive = Join-Path $root '.wdk\wdk.zip'
    New-Item -ItemType Directory -Force (Split-Path $archive) | Out-Null
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$wdkPackage/$wdkPackageVersion/$wdkPackage.$wdkPackageVersion.nupkg" -OutFile $archive
    Expand-Archive $archive (Join-Path $root '.wdk\wdk')
    Remove-Item $archive
}

$out = Join-Path $PSScriptRoot 'out'
$obj = Join-Path $out 'obj'
$pkg = Join-Path $out 'pkg'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $obj, $pkg | Out-Null

# Import the MSVC x64 environment.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC build tools not found' }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}

# hidport.h lives among the kernel headers; stage it alone so that the rest of
# the km directory cannot shadow user-mode SDK headers.
Copy-Item "$wdk\Include\$sdkVersion\km\hidport.h" $obj

$minor = $umdfVersion.Split('.')[1]
$sources = 'winstadia.c', 'stadia.c', 'usb.c', 'bluetooth.c' | ForEach-Object { "$PSScriptRoot\$_" }
& cl /nologo /W4 /O2 /MT /LD /std:c17 `
    /D UMDF_VERSION_MAJOR=2 /D UMDF_VERSION_MINOR=$minor /D UMDF_USING_NTSTATUS `
    /D UNICODE /D _UNICODE /D _WIN32_WINNT=0x0A00 `
    /I "$wdk\Include\wdf\umdf\$umdfVersion" /I $obj `
    /Fo"$obj\\" /Fe"$pkg\winstadia.dll" $sources `
    /link /NOIMPLIB /NOEXP "$wdk\Lib\wdf\umdf\x64\$umdfVersion\WdfDriverStubUm.lib" ntdll.lib
if ($LASTEXITCODE) { throw 'compiling failed' }
Copy-Item "$PSScriptRoot\winstadia.inf" $pkg
# Windows only replaces an installed package when DriverVer is newer, so every
# build gets the current date and a time-based version.
& "$wdk\bin\$sdkVersion\x64\stampinf.exe" -f "$pkg\winstadia.inf" -d * -v * | Out-Null
if ($LASTEXITCODE) { throw 'stampinf failed' }

Write-Host "Driver package ready: $pkg"
