[CmdletBinding()]
param(
    [string]$Drive,
    [string]$OutputRoot,
    [string]$WslDistro = "Ubuntu-24.04"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $repoRoot "results\a1099-battery-logs"
}

function Resolve-DeviceRoot {
    param([string]$RequestedDrive)

    if ($RequestedDrive) {
        if ($RequestedDrive -match '^[A-Za-z]:?$') {
            return "{0}:\" -f $RequestedDrive.Substring(0, 1).ToUpperInvariant()
        }
        return [IO.Path]::GetFullPath($RequestedDrive)
    }

    $matches = @(
        Get-PSDrive -PSProvider FileSystem | ForEach-Object {
            $root = $_.Root
            if ((Test-Path -LiteralPath (Join-Path $root ".rockbox")) -and
                ((Test-Path -LiteralPath (Join-Path $root "iPod_Control")) -or
                 (Test-Path -LiteralPath (Join-Path $root "a1099_battery_model.csv")))) {
                $root
            }
        }
    )
    if ($matches.Count -eq 0) {
        throw "No mounted iPod was found. Connect it once, wait for a drive letter, and run this command again."
    }
    if ($matches.Count -gt 1) {
        throw "More than one Rockbox device was found: $($matches -join ', '). Re-run with -Drive E (using the iPod's drive letter)."
    }
    return $matches[0]
}

function Convert-ToWslPath {
    param([string]$WindowsPath)

    $full = [IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^[A-Za-z]:\\') {
        throw "Cannot convert non-drive path to WSL: $full"
    }
    $letter = $full.Substring(0, 1).ToLowerInvariant()
    $tail = $full.Substring(2).Replace('\', '/')
    return "/mnt/$letter$tail"
}

$deviceRoot = Resolve-DeviceRoot $Drive
if (-not (Test-Path -LiteralPath (Join-Path $deviceRoot ".rockbox"))) {
    throw "The selected drive does not contain a .rockbox directory: $deviceRoot"
}
$rockboxInfo = Join-Path $deviceRoot ".rockbox\rockbox-info.txt"
if (-not (Test-Path -LiteralPath $rockboxInfo)) {
    throw "The selected Rockbox device has no rockbox-info.txt, so its target cannot be verified."
}
$targetLine = Get-Content -LiteralPath $rockboxInfo | Where-Object {
    $_ -match '^Target:'
} | Select-Object -First 1
if ($targetLine -ne "Target: ipodcolor") {
    throw "Refusing non-A1099 target '$targetLine'. This collector is only for the iPod Photo/Color target."
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$destination = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) $timestamp
New-Item -ItemType Directory -Path $destination -Force | Out-Null

$files = @(
    @{ Source = "a1099_battery_model.csv"; Destination = "a1099_battery_model.csv" },
    @{ Source = "battery_bench.txt"; Destination = "battery_bench.txt" },
    @{ Source = ".rockbox\config.cfg"; Destination = "rockbox-config.cfg" },
    @{ Source = ".rockbox\rockbox-info.txt"; Destination = "rockbox-info.txt" },
    @{ Source = ".rockbox\battery_levels.cfg"; Destination = "battery_levels.cfg" },
    @{ Source = ".rockbox\battery_levels.default"; Destination = "battery_levels.default" },
    @{ Source = ".rockbox\logf.txt"; Destination = "logf.txt" },
    @{ Source = ".rockbox\playback.log"; Destination = "playback.log" },
    @{ Source = ".rockbox\pp5020-perf.log"; Destination = "pp5020-perf.log" }
)

$copied = @()
foreach ($file in $files) {
    $source = Join-Path $deviceRoot $file.Source
    if (Test-Path -LiteralPath $source) {
        $target = Join-Path $destination $file.Destination
        Copy-Item -LiteralPath $source -Destination $target
        $copied += Get-Item -LiteralPath $target
    }
}

$telemetry = Join-Path $destination "a1099_battery_model.csv"
if (-not (Test-Path -LiteralPath $telemetry)) {
    throw "The iPod is mounted, but a1099_battery_model.csv is missing. Install the test build and start Battery Benchmark once before collecting."
}

$hashes = @(
    $copied | ForEach-Object {
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
        [PSCustomObject]@{
            File = $_.Name
            Bytes = $_.Length
            SHA256 = $hash.Hash
            LastWriteTimeUtc = $_.LastWriteTimeUtc.ToString("o")
        }
    }
)
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination "files.json") -Encoding UTF8

$driveInfo = Get-PSDrive -Name $deviceRoot.Substring(0, 1) -ErrorAction SilentlyContinue
[PSCustomObject]@{
    CollectedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    DeviceRoot = $deviceRoot
    VolumeName = if ($driveInfo) { $driveInfo.Description } else { $null }
    UsedBytes = if ($driveInfo) { $driveInfo.Used } else { $null }
    FreeBytes = if ($driveInfo) { $driveInfo.Free } else { $null }
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination "device.json") -Encoding UTF8

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "Logs were copied to $destination, but WSL is unavailable for analysis."
}

$analyzer = Convert-ToWslPath (Join-Path $PSScriptRoot "analyze_a1099_battery_log.py")
$telemetryWsl = Convert-ToWslPath $telemetry
$markdownWsl = Convert-ToWslPath (Join-Path $destination "report.md")
$jsonWsl = Convert-ToWslPath (Join-Path $destination "report.json")

& wsl.exe -d $WslDistro -- python3 $analyzer $telemetryWsl --markdown $markdownWsl --json $jsonWsl
if ($LASTEXITCODE -ne 0) {
    throw "Logs were copied to $destination, but the analyzer exited with code $LASTEXITCODE."
}

Write-Host "A1099 logs copied and analyzed."
Write-Host "Report: $(Join-Path $destination 'report.md')"
Write-Host "Keep the iPod connected; Codex can read this folder directly."
