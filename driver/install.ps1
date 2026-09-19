# Installs (or with -Uninstall removes) the driver package built by build.ps1:
# trusts the self-signed certificate on this machine and puts the driver on
# Stadia controllers, connected now or later.
# Elevates itself; output goes to out\install.log.
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Security

$out = Join-Path $PSScriptRoot 'out'
$inf = Join-Path $out 'pkg\winstadia.inf'
$cer = Join-Path $out 'winstadia.cer'
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

    if ($Uninstall) {
        # Returns the controller to the inbox HID driver.
        Get-ChildItem "$env:windir\INF\oem*.inf" |
            Where-Object { Select-String -Path $_ -Pattern 'winstadia\.dll' -Quiet } |
            ForEach-Object { pnputil /delete-driver $_.Name /uninstall }
        foreach ($store in $stores) {
            Remove-Item "$store\$thumbprint" -ErrorAction SilentlyContinue
        }
    } else {
        foreach ($store in $stores) {
            Import-Certificate -FilePath $cer -CertStoreLocation $store | Out-Null
        }
        pnputil /add-driver $inf /install
        "pnputil exit code: $LASTEXITCODE"
        if ($LASTEXITCODE -eq 3010) {
            # Something held the controller open, so it could not restart.
            'Unplug the controller and plug it back in to finish.'
        }
    }
} catch {
    "FAILED: $_"
} finally {
    Stop-Transcript | Out-Null
}
