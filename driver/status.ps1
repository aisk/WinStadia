# Asks the driver what it is doing, through the status feature report of the
# controller it presents. With -Watch it keeps printing the raw Stadia input
# reports as they change, until Ctrl+C.
param([switch]$Watch)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class WinStadiaHid {
    [StructLayout(LayoutKind.Sequential)]
    public struct Attributes { public int Size; public ushort VendorId; public ushort ProductId; public ushort Version; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition,
                                            uint flags, IntPtr template);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
    [DllImport("hid.dll")]
    public static extern void HidD_GetHidGuid(out Guid guid);
    [DllImport("hid.dll")]
    public static extern bool HidD_GetAttributes(IntPtr handle, ref Attributes attributes);
    [DllImport("hid.dll", CharSet = CharSet.Unicode)]
    public static extern bool HidD_GetSerialNumberString(IntPtr handle, StringBuilder buffer, int length);
    [DllImport("hid.dll", SetLastError = true)]
    public static extern bool HidD_GetFeature(IntPtr handle, byte[] report, int length);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern int CM_Get_Device_Interface_List_SizeW(out uint length, ref Guid guid, string id, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern int CM_Get_Device_Interface_ListW(ref Guid guid, string id, char[] buffer, uint length,
                                                           uint flags);
}
'@

$vendorId = 0x045E
$productId = 0x02FD
# Tells the presented controller apart from a real Xbox controller.
$serial = 'WinStadia'
$statusReportId = 0xE1
$statusReportLength = 64
$invalidHandle = [IntPtr](-1)

function Open-Pad {
    $guid = [Guid]::Empty
    [WinStadiaHid]::HidD_GetHidGuid([ref]$guid)
    $length = 0
    [void][WinStadiaHid]::CM_Get_Device_Interface_List_SizeW([ref]$length, [ref]$guid, $null, 0)
    $buffer = New-Object char[] $length
    [void][WinStadiaHid]::CM_Get_Device_Interface_ListW([ref]$guid, $null, $buffer, $length, 0)

    foreach ($path in (-join $buffer).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
        # No access rights are needed for feature reports.
        $handle = [WinStadiaHid]::CreateFileW($path, 0, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
        if ($handle -eq $invalidHandle) { continue }

        $attributes = New-Object WinStadiaHid+Attributes
        $attributes.Size = [Runtime.InteropServices.Marshal]::SizeOf($attributes)
        $name = New-Object Text.StringBuilder 64
        if ([WinStadiaHid]::HidD_GetAttributes($handle, [ref]$attributes) -and
            $attributes.VendorId -eq $vendorId -and $attributes.ProductId -eq $productId -and
            [WinStadiaHid]::HidD_GetSerialNumberString($handle, $name, 2 * $name.Capacity) -and
            $name.ToString() -ceq $serial) {
            return $handle
        }
        [void][WinStadiaHid]::CloseHandle($handle)
    }
    throw 'WinStadia controller not found, is the controller connected and the driver installed?'
}

# Status report: [1] last rumble write failed, [2..5] last error NTSTATUS
# (little endian), [6] length of the last raw Stadia input report, [7..] that
# report.
function Read-Status($handle) {
    $report = New-Object byte[] $statusReportLength
    $report[0] = $statusReportId
    if (-not [WinStadiaHid]::HidD_GetFeature($handle, $report, $report.Length)) {
        throw "reading the status report failed, error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $raw = if ($report[6] -gt 0) { $report[7..(6 + $report[6])] } else { @() }
    [pscustomobject]@{
        RumbleFailed = $report[1] -ne 0
        Error        = [BitConverter]::ToUInt32($report, 2)
        RawReport    = ($raw | ForEach-Object { $_.ToString('X2') }) -join ' '
    }
}

$pad = Open-Pad
try {
    $status = Read-Status $pad
    $text = if ($status.RawReport) { 'running, receiving input' } else { 'running, no input received yet' }
    if ($status.RumbleFailed) { $text += ', last rumble write failed' }
    if ($status.Error) { $text += ', last error 0x{0:X8}' -f $status.Error }
    "Driver: $text"

    if ($Watch) {
        'Raw input reports, press Ctrl+C to stop.'
        $last = ''
        while ($true) {
            $raw = (Read-Status $pad).RawReport
            if ($raw -and $raw -ne $last) {
                $raw
                $last = $raw
            }
            Start-Sleep -Milliseconds 4
        }
    }
} finally {
    [void][WinStadiaHid]::CloseHandle($pad)
}
