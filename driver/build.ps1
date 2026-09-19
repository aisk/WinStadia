# Builds, packages and signs the UMDF driver into driver\out\pkg.
# Uses the NuGet WDK unpacked in .wdk (no system-wide WDK install needed).
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Security

$root = Split-Path $PSScriptRoot -Parent
$wdk = Join-Path $root '.wdk\wdk\c'
$sdkVersion = '10.0.26100.0'
$umdfVersion = '2.31'
$certSubject = 'CN=winstadia driver signing'

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
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}

# hidport.h lives among the kernel headers; stage it alone so that the rest of
# the km directory cannot shadow user-mode SDK headers.
Copy-Item "$wdk\Include\$sdkVersion\km\hidport.h" $obj

$minor = $umdfVersion.Split('.')[1]
$sources = 'winstadia.c', 'stadia.c' | ForEach-Object { "$PSScriptRoot\$_" }
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
& "$wdk\bin\$sdkVersion\x86\Inf2Cat.exe" /driver:$pkg /os:10_X64 /uselocaltime
if ($LASTEXITCODE) { throw 'Inf2Cat failed' }

# Self-signed code signing certificate; install.ps1 makes this machine trust it.
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq $certSubject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(10)
}
Export-Certificate -Cert $cert -FilePath "$out\winstadia.cer" | Out-Null

& signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint (Get-ChildItem $pkg -Include *.dll, *.cat -Recurse).FullName
if ($LASTEXITCODE) { throw 'signing failed' }

Write-Host "Driver package ready: $pkg"
