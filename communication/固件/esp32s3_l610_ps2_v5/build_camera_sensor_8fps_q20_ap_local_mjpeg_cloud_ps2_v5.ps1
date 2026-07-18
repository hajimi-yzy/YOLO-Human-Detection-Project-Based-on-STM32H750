param()

$ErrorActionPreference = 'Stop'
$IdfExport = 'C:\path\to\esp-idf-v5.5.1\export.ps1'
$StageRoot = Join-Path $env:LOCALAPPDATA 'Espressif\staging'
$StageProject = Join-Path $StageRoot 'L610_RNDIS_CAMERA_SENSOR_8FPS_Q20_AP_LOCAL_MJPEG_CLOUD_PS2_V5'
$ArtifactDir = Join-Path $PSScriptRoot 'firmware_binaries\camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5'

if (-not (Test-Path -LiteralPath $IdfExport)) {
    throw "ESP-IDF 5.5.1 was not found at $IdfExport"
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
    throw "Failed to synchronize local MJPEG + cloud + PS2 v5 source (robocopy code $LASTEXITCODE)"
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
Copy-Item -LiteralPath (Join-Path $StageProject 'build\bootloader\bootloader.bin') -Destination $ArtifactDir
Copy-Item -LiteralPath (Join-Path $StageProject 'build\partition_table\partition-table.bin') -Destination $ArtifactDir
Copy-Item -LiteralPath (Join-Path $StageProject 'build\bootloader\bootloader.bin') -Destination (Join-Path $ArtifactDir 'bootloader')
Copy-Item -LiteralPath (Join-Path $StageProject 'build\partition_table\partition-table.bin') -Destination (Join-Path $ArtifactDir 'partition_table')
Copy-Item -LiteralPath (Join-Path $StageProject 'build\flasher_args.json') -Destination $ArtifactDir
Copy-Item -LiteralPath (Join-Path $StageProject 'sdkconfig') -Destination (Join-Path $ArtifactDir 'sdkconfig.v1-8fps-q20-ap-local-mjpeg-cloud-ps2-v5')

Write-Host "Camera + cloud UDP + local MJPEG + PS2 UART v5 firmware copied to $ArtifactDir"
