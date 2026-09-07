<#
.SYNOPSIS
  Extracts one length-prefixed PE file embedded as a managed resource inside a
  published CloudRedirect.exe (framework-dependent single-file bundle).

.DESCRIPTION
  ui/native/steam_api.dll is intentionally not tracked in git. Every upstream
  release embeds it, so CI pulls it out of a pinned upstream CloudRedirect.exe
  and verifies its SHA-256 before use. Managed resources are stored as
  [uint32 length][bytes], so each embedded PE is found by locating an "MZ"
  header whose preceding 4 bytes give a plausible length and whose PE header
  validates. Nothing is decompressed: framework-dependent bundles store
  resources verbatim.

.PARAMETER Exe          Path to CloudRedirect.exe to scan.
.PARAMETER OutFile      Where to write the extracted file.
.PARAMETER Machine      IMAGE_FILE_HEADER.Machine to match (0x14C = i386, 0x8664 = x64).
.PARAMETER Dll          Match DLLs; omit to match EXEs.
.PARAMETER ExpectedSha256  Required SHA-256 of the extracted file (lowercase hex).
#>
param(
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [string] $OutFile,
    [Parameter(Mandatory)] [int] $Machine,
    [switch] $Dll,
    [Parameter(Mandatory)] [string] $ExpectedSha256
)
$ErrorActionPreference = 'Stop'

$bytes = [IO.File]::ReadAllBytes($Exe)
# Latin-1 maps every byte to one char, so IndexOf gives fast byte searching.
$text = [Text.Encoding]::Latin1.GetString($bytes)
$pos = 4
$matches = @()
while (($i = $text.IndexOf('MZ', $pos, [StringComparison]::Ordinal)) -ge 0) {
    $pos = $i + 2
    if ($i + 0x40 -ge $bytes.Length) { continue }
    $len = [BitConverter]::ToUInt32($bytes, $i - 4)
    if ($len -lt 1024 -or $len -gt ($bytes.Length - $i)) { continue }
    $lfanew = [BitConverter]::ToInt32($bytes, $i + 0x3C)
    if ($lfanew -le 0 -or $lfanew + 24 -ge $len) { continue }
    $p = $i + $lfanew
    if ($bytes[$p] -ne 0x50 -or $bytes[$p+1] -ne 0x45 -or $bytes[$p+2] -ne 0 -or $bytes[$p+3] -ne 0) { continue }
    $m = [BitConverter]::ToUInt16($bytes, $p + 4)
    $characteristics = [BitConverter]::ToUInt16($bytes, $p + 22)
    $isDll = ($characteristics -band 0x2000) -ne 0
    if ($m -ne $Machine -or $isDll -ne $Dll.IsPresent) { continue }
    $matches += [pscustomobject]@{ Offset = $i; Length = $len }
}

foreach ($match in $matches) {
    $slice = New-Object byte[] $match.Length
    [Array]::Copy($bytes, $match.Offset, $slice, 0, $match.Length)
    $sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($slice)).Replace('-', '').ToLowerInvariant()
    if ($sha -eq $ExpectedSha256.ToLowerInvariant()) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $OutFile) | Out-Null
        [IO.File]::WriteAllBytes($OutFile, $slice)
        Write-Host "Extracted $OutFile ($($match.Length) bytes, sha256 $sha)"
        exit 0
    }
    Write-Host "Skipping candidate at offset $($match.Offset): sha256 $sha does not match"
}
Write-Error "No embedded PE in $Exe matched machine=0x$('{0:X}' -f $Machine) dll=$Dll sha256=$ExpectedSha256"
exit 1
