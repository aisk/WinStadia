# Installs (or with -Uninstall removes) the driver package, built by build.ps1
# or taken from a release: signs it for this machine and puts the driver on
# Stadia controllers, connected now or later.
# Elevates itself; output goes to install.log next to the package.
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Security

# A release has the package next to this script, a checkout has it in the
# build output.
$pkg = Join-Path $PSScriptRoot 'pkg'
if (-not (Test-Path $pkg)) { $pkg = Join-Path $PSScriptRoot 'out\pkg' }
$inf = Join-Path $pkg 'winstadia.inf'
$cat = Join-Path $pkg 'winstadia.cat'
$log = Join-Path (Split-Path $pkg -Parent) 'install.log'
$certSubject = 'CN=WinStadia driver signing'
$stores = 'Root', 'TrustedPublisher'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = [Security.Principal.WindowsBuiltInRole]::Administrator
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole($admin)) {
    $arguments = '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`""
    if ($Uninstall) { $arguments += '-Uninstall' }
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList $arguments
    Get-Content $log
    return
}

# Driver packages of this driver in the driver store, whatever their version.
function Get-InstalledPackage {
    Get-ChildItem "$env:windir\INF\oem*.inf" |
        Where-Object { Select-String -Path $_ -Pattern 'winstadia\.dll' -Quiet }
}

# Takes the trust away from this driver's certificates, from all of them or
# from those the filter picks.
function Remove-Trust([scriptblock]$filter = { $true }) {
    foreach ($store in $stores) {
        Get-ChildItem "Cert:\LocalMachine\$store" |
            Where-Object { $_.Subject -eq $certSubject } |
            Where-Object $filter |
            Remove-Item
    }
}

# Windows wants driver packages signed by someone the machine trusts. The
# certificate for that is made here and trusted only here, and its private key
# is gone again once the package is signed, so nothing else can ever be signed
# with it.
function Add-Signature {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject `
        -CertStoreLocation Cert:\LocalMachine\My -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(30)
    try {
        # Signing checks the result, so the trust has to be there first.
        $public = New-Object Security.Cryptography.X509Certificates.X509Certificate2 (, $cert.RawData)
        foreach ($store in $stores) {
            $target = New-Object Security.Cryptography.X509Certificates.X509Store $store, 'LocalMachine'
            $target.Open('ReadWrite')
            $target.Add($public)
            $target.Close()
        }

        Remove-Item $cat -ErrorAction SilentlyContinue
        New-FileCatalog -Path $pkg -CatalogFilePath $cat -CatalogVersion 2 | Out-Null
        $signature = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
        if ($signature.Status -ne 'Valid') { throw "signing failed: $($signature.StatusMessage)" }
    } finally {
        Remove-Item $cert.PSPath -DeleteKey
    }
    $cert.Thumbprint
}

Start-Transcript $log -Force | Out-Null
try {
    if ($Uninstall) {
        # Returns the controller to the inbox HID driver.
        Get-InstalledPackage | ForEach-Object { pnputil /delete-driver $_.Name /uninstall }
        Remove-Trust
    } else {
        if (-not (Test-Path $inf)) { throw "no driver package in $pkg, run build.ps1 first" }
        $thumbprint = Add-Signature
        $previous = @(Get-InstalledPackage)
        pnputil /add-driver $inf /install
        $exitCode = $LASTEXITCODE
        "pnputil exit code: $exitCode"
        if (@(Get-InstalledPackage).Count -gt $previous.Count) {
            # Earlier versions would pile up in the driver store. A controller
            # that is not connected right now is still on one, and moves to the
            # new package with this.
            $previous | ForEach-Object { pnputil /delete-driver $_.Name /uninstall }
            Remove-Trust { $_.Thumbprint -ne $thumbprint }
        } else {
            # The same build was in the store already, under the certificate
            # it got back then.
            Remove-Trust { $_.Thumbprint -eq $thumbprint }
        }
        if ($exitCode -eq 3010) {
            # Something held the controller open, so it could not restart.
            'Reconnect the controller to finish. Over Bluetooth, switch Bluetooth off and on.'
        }
    }
} catch {
    "FAILED: $_"
} finally {
    Stop-Transcript | Out-Null
}
