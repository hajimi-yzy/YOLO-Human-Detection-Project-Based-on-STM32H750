param(
    [ValidatePattern('^v7(?:_[a-z0-9]+)*$')]
    [string]$Revision = 'v7',

    [Parameter(Mandatory = $true)]
    [string]$IdfExport
)

$ErrorActionPreference = 'Stop'
$StageRoot = Join-Path $env:LOCALAPPDATA 'Espressif\staging'
$StageProject = Join-Path $StageRoot (
    'L610_RNDIS_CAMERA_SENSOR_WIFI_MANAGER_' + $Revision.ToUpperInvariant())
$ArtifactDir = Join-Path $PSScriptRoot (
    'firmware_binaries\camera_sensor_4g_wifi_' + $Revision)

if (-not (Test-Path -LiteralPath $IdfExport)) {
    throw "ESP-IDF export script was not found at $IdfExport"
}

$stageRootFull = [IO.Path]::GetFullPath($StageRoot).TrimEnd('\') + '\'
$stageProjectFull = [IO.Path]::GetFullPath($StageProject).TrimEnd('\') + '\'
if (-not $stageProjectFull.StartsWith($stageRootFull, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe staging path: $StageProject"
}
if (Test-Path -LiteralPath $ArtifactDir) {
    throw "Refusing to overwrite existing firmware artifact: $ArtifactDir"
}

if (Test-Path -LiteralPath $StageProject) {
    Remove-Item -LiteralPath $StageProject -Recurse -Force
}
New-Item -ItemType Directory -Path $StageProject -Force | Out-Null

& robocopy.exe $PSScriptRoot $StageProject /MIR /XD build managed_components firmware_binaries .git /XF sdkconfig sdkconfig.old dependencies.lock 'build-*.log' /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) {
    throw "Failed to synchronize Wi-Fi manager source (robocopy code $LASTEXITCODE)"
}

Set-Location -LiteralPath $StageProject
. $IdfExport
$env:SDKCONFIG_DEFAULTS = 'sdkconfig.defaults;sdkconfig.v1-8fps-q20-ap-napt.defaults'
idf.py set-target esp32s3
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
idf.py build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Path $ArtifactDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ArtifactDir 'bootloader') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ArtifactDir 'partition_table') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $StageProject 'build\usb_rndis_4g_module.bin') -Destination $ArtifactDir
Copy-Item -LiteralPath (Join-Path $StageProject 'build\bootloader\bootloader.bin') -Destination (Join-Path $ArtifactDir 'bootloader')
Copy-Item -LiteralPath (Join-Path $StageProject 'build\partition_table\partition-table.bin') -Destination (Join-Path $ArtifactDir 'partition_table')
Copy-Item -LiteralPath (Join-Path $StageProject 'build\flasher_args.json') -Destination $ArtifactDir

Write-Host "V7 firmware copied to $ArtifactDir"
