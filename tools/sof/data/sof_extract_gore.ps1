# sof_extract_gore.ps1 - pulls the files needed to test SoF's gore (wound zones,
# dismemberment) out of your own pak0.pak into sof_gore.zip on your Desktop (about 8 MB):
# two full-gore character packs (the tutorial ones have the gore stripped out) and the
# gore/impact sounds. Reads the pak only; nothing is changed.
#
# Usage: right-click this file -> "Run with PowerShell", then pick pak0.pak
#        (it is in the "base" folder of your Soldier of Fortune install).
param([string]$Pak)

$ErrorActionPreference = 'Stop'

$prefixes = @('sound/impact/', 'effects/gore/')
$single   = @('ghoul/enemy/meso/meso_tsr2.ghb', 'ghoul/enemy/ecto/ecto_tsr1.ghb')

function Test-Wanted([string]$n) {
    $n = $n.ToLowerInvariant().Replace('\', '/')
    foreach ($p in $prefixes) { if ($n.StartsWith($p)) { return $true } }
    return ($single -contains $n)
}

function Select-Pak {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title = 'Pick pak0.pak from your Soldier of Fortune "base" folder'
        $dlg.Filter = 'SoF pak files (*.pak)|*.pak|All files (*.*)|*.*'
        $dlg.FileName = 'pak0.pak'
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.FileName }
        return $null
    } catch {
        return (Read-Host 'Full path to pak0.pak').Trim('"', ' ')
    }
}

$fs = $null; $zfs = $null; $zip = $null
try {
    if (-not $Pak) { $Pak = Select-Pak }
    if (-not $Pak) { throw 'No pak file picked.' }
    if (-not (Test-Path -LiteralPath $Pak)) { throw "File not found: $Pak" }

    Add-Type -AssemblyName System.IO.Compression
    $desktop = [Environment]::GetFolderPath('Desktop')
    if (-not $desktop -or -not (Test-Path -LiteralPath $desktop)) { $desktop = (Get-Location).Path }
    $out = Join-Path $desktop 'sof_gore.zip'
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out }

    $fs = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Pak).Path)
    $hdr = New-Object byte[] 12
    if ($fs.Read($hdr, 0, 12) -ne 12) { throw 'not a pak file' }
    if ([System.Text.Encoding]::ASCII.GetString($hdr, 0, 4) -ne 'PACK') { throw 'not a pak file' }
    $ofs = [BitConverter]::ToInt32($hdr, 4)
    $len = [BitConverter]::ToInt32($hdr, 8)

    $dir = New-Object byte[] $len
    $fs.Position = $ofs
    $got = 0
    while ($got -lt $len) {
        $r = $fs.Read($dir, $got, $len - $got)
        if ($r -le 0) { throw 'pak directory is cut short' }
        $got += $r
    }

    $zfs = [System.IO.File]::Create($out)
    $zip = New-Object System.IO.Compression.ZipArchive($zfs, [System.IO.Compression.ZipArchiveMode]::Create)
    $count = 0
    $entries = [Math]::Floor($len / 64)
    Write-Host "Scanning $entries files in $Pak ..."
    for ($i = 0; $i -lt $entries; $i++) {
        $b = $i * 64
        $end = $b
        while ($end -lt $b + 56 -and $dir[$end] -ne 0) { $end++ }
        $name = [System.Text.Encoding]::ASCII.GetString($dir, $b, $end - $b)
        if (-not (Test-Wanted $name)) { continue }
        $pos  = [BitConverter]::ToInt32($dir, $b + 56)
        $size = [BitConverter]::ToInt32($dir, $b + 60)

        $data = New-Object byte[] $size
        $fs.Position = $pos
        $got = 0
        while ($got -lt $size) {
            $r = $fs.Read($data, $got, $size - $got)
            if ($r -le 0) { throw "file cut short in pak: $name" }
            $got += $r
        }
        $entry = $zip.CreateEntry($name.Replace('\', '/'), [System.IO.Compression.CompressionLevel]::Optimal)
        $s = $entry.Open()
        $s.Write($data, 0, $size)
        $s.Dispose()
        $count++
    }
    $zip.Dispose(); $zip = $null
    $zfs.Dispose(); $zfs = $null
    Write-Host ""
    Write-Host "Done: wrote $out with $count files" -ForegroundColor Green
    if ($count -eq 0) { Write-Host 'No matching files - is that the right pak0.pak?' -ForegroundColor Yellow }
} catch {
    Write-Host ""
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($zip) { $zip.Dispose() }
    if ($zfs) { $zfs.Dispose() }
    if ($fs)  { $fs.Dispose() }
}
Read-Host 'Press Enter to close'
