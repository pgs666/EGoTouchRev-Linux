# Read firmware tables through Kernel32. No driver loading or hardware writes.
# GetSystemFirmwareTable returns only the first table of each signature;
# duplicate SSDTs are reported as PARTIAL, never as a complete firmware dump.
param(
    [string]$OutputDir = (Join-Path ([Environment]::GetFolderPath('Desktop')) `
        ("GaokunAcpi-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Run this script on Windows.' }
if ((Test-Path -LiteralPath $OutputDir) -or (Test-Path -LiteralPath "$OutputDir.zip")) {
    throw 'Choose a new output directory; existing evidence is not overwritten.'
}
$OutputDir = (New-Item -ItemType Directory -Path $OutputDir).FullName

if (-not ('Himax.AcpiCapture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace Himax {
    public static class AcpiCapture {
        const uint Provider = 0x41435049; // 'ACPI', unlike little-endian table IDs.
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint EnumSystemFirmwareTables(uint provider, IntPtr data, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint GetSystemFirmwareTable(uint provider, uint id, IntPtr data, uint size);
        public static byte[] Read(uint id, bool enumerate) {
            uint size = enumerate ? EnumSystemFirmwareTables(Provider, IntPtr.Zero, 0)
                                  : GetSystemFirmwareTable(Provider, id, IntPtr.Zero, 0);
            if (size == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (size > 4 * 1024 * 1024) throw new Exception("Firmware response exceeds 4 MiB.");
            IntPtr data = Marshal.AllocHGlobal((int)size);
            try {
                uint count = enumerate ? EnumSystemFirmwareTables(Provider, data, size)
                                       : GetSystemFirmwareTable(Provider, id, data, size);
                if (count == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
                if (count > size) throw new Exception("Firmware response changed size; retry capture.");
                byte[] result = new byte[count];
                Marshal.Copy(data, result, 0, (int)count);
                return result;
            } finally { Marshal.FreeHGlobal(data); }
        }
    }
}
'@
}

$manifest = [ordered]@{
    schema = 1
    capturedUtc = [DateTime]::UtcNow.ToString('o')
    status = 'FAILED'
    limitation = 'Only the first ACPI table per signature is accessible through this API.'
    enumerationError = $null
    signatures = @{}
    tables = @()
}
try {
    $ids = [Himax.AcpiCapture]::Read(0, $true)
    if (($ids.Length % 4) -ne 0) { throw 'Invalid firmware table enumeration length.' }
    for ($offset = 0; $offset -lt $ids.Length; $offset += 4) {
        $name = [Text.Encoding]::ASCII.GetString($ids, $offset, 4)
        if (-not $manifest.signatures.ContainsKey($name)) { $manifest.signatures[$name] = 0 }
        $manifest.signatures[$name]++
    }
} catch { $manifest.enumerationError = $_.Exception.Message }

# DSDT is explicitly requested even if the provider omitted it from enumeration.
$names = @('DSDT')
foreach ($name in @('SSDT', 'FACP')) {
    if ($manifest.signatures.ContainsKey($name)) { $names += $name }
}
foreach ($name in $names) {
    $entry = [ordered]@{
        signature = $name; file = $null; bytes = 0; sha256 = $null; valid = $false; error = $null
    }
    try {
        $id = [BitConverter]::ToUInt32([Text.Encoding]::ASCII.GetBytes($name), 0)
        $data = [Himax.AcpiCapture]::Read($id, $false)
        $file = Join-Path $OutputDir "$name.dat"
        [IO.File]::WriteAllBytes($file, $data)
        $entry.file = "$name.dat"
        $entry.bytes = $data.Length
        $entry.sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        if ($data.Length -lt 36 -or [Text.Encoding]::ASCII.GetString($data, 0, 4) -ne $name) {
            throw 'Missing or mismatched ACPI header.'
        }
        if ([BitConverter]::ToUInt32($data, 4) -ne $data.Length) { throw 'ACPI length mismatch.' }
        $sum = 0
        foreach ($byte in $data) { $sum = ($sum + [int]$byte) -band 255 }
        if ($sum -ne 0) { throw 'ACPI checksum mismatch.' }
        $entry.valid = $true
    } catch { $entry.error = $_.Exception.Message }
    $manifest.tables += $entry
}
$validDsdt = @($manifest.tables | Where-Object { $_.signature -eq 'DSDT' -and $_.valid }).Count -eq 1
$duplicates = @($manifest.signatures.GetEnumerator() | Where-Object { $_.Key -eq 'SSDT' -and $_.Value -gt 1 }).Count -gt 0
$failures = @($manifest.tables | Where-Object { -not $_.valid }).Count -gt 0
if ($validDsdt) {
    $manifest.status = 'OK'
    if ($duplicates -or $failures -or $manifest.enumerationError) { $manifest.status = 'PARTIAL' }
}
[IO.File]::WriteAllText((Join-Path $OutputDir 'manifest.json'), `
    ($manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
Compress-Archive -LiteralPath $OutputDir -DestinationPath "$OutputDir.zip"
Write-Output ("CAPTURE_STATUS=" + $manifest.status)
Write-Output ("ARCHIVE=" + "$OutputDir.zip")
if ($manifest.status -eq 'FAILED') { exit 1 }
if ($manifest.status -eq 'PARTIAL') { exit 2 }
exit 0
