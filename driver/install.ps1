# Installs (or with -Uninstall removes) the driver package built by build.ps1:
# trusts the self-signed certificate on this machine and creates the
# root-enumerated virtual device. Elevates itself; output goes to out\install.log.
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Security

$hardwareId = 'root\winstadia'
$out = Join-Path $PSScriptRoot 'out'
$inf = Join-Path $out 'pkg\winstadia.inf'
$cer = Join-Path $out 'winstadia.cer'
$devcon = Join-Path (Split-Path $PSScriptRoot -Parent) '.wdk\wdk\c\tools\10.0.26100.0\x64\devcon.exe'
$stores = 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = [Security.Principal.WindowsBuiltInRole]::Administrator
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole($admin)) {
    $arguments = '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`""
    if ($Uninstall) { $arguments += '-Uninstall' }
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList $arguments
    Get-Content (Join-Path $out 'install.log')
    return
}

Start-Transcript (Join-Path $out 'install.log') -Force | Out-Null
try {
    $thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer).Thumbprint
    $installed = & $devcon hwids "@ROOT\HIDCLASS\*" | Select-String -SimpleMatch $hardwareId -Quiet

    if ($Uninstall) {
        if ($installed) { & $devcon remove $hardwareId }
        Get-ChildItem "$env:windir\INF\oem*.inf" |
            Where-Object { Select-String -Path $_ -SimpleMatch $hardwareId -Quiet } |
            ForEach-Object { pnputil /delete-driver $_.Name /uninstall /force }
        foreach ($store in $stores) {
            Remove-Item "$store\$thumbprint" -ErrorAction SilentlyContinue
        }
    } else {
        foreach ($store in $stores) {
            Import-Certificate -FilePath $cer -CertStoreLocation $store | Out-Null
        }
        if ($installed) {
            & $devcon update $inf $hardwareId
        } else {
            & $devcon install $inf $hardwareId
        }
    }
    "devcon exit code: $LASTEXITCODE"
} catch {
    "FAILED: $_"
} finally {
    Stop-Transcript | Out-Null
}
