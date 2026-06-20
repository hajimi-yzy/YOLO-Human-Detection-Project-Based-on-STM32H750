$ErrorActionPreference = 'Stop'

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

$required = @(
  'Core/User/Inc/app_camera_ai.h',
  'Core/User/Src/app_camera_ai.c',
  'Core/User/Inc/nanov4_postprocess.h',
  'Core/User/Src/nanov4_postprocess.c'
)

foreach ($relativePath in $required) {
  $fullPath = Join-Path $projectRoot $relativePath
  if (-not (Test-Path -Path $fullPath -PathType Leaf)) {
    throw "Missing boundary file: $relativePath"
  }
}

$mainPath = Join-Path $projectRoot 'Core/Src/main.c'
$aiPath = Join-Path $projectRoot 'X-CUBE-AI/App/app_x-cube-ai.c'

$main = Get-Content $mainPath -Raw
$ai = Get-Content $aiPath -Raw
$commentPattern = '(?s)/\*.*?\*/|(?m)//.*$'

function Remove-CComments {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  return [regex]::Replace($Text, $commentPattern, '')
}

$mainClean = Remove-CComments $main
$aiClean = Remove-CComments $ai

if (-not [regex]::IsMatch($mainClean, '(?m)^\s*#\s*define\s+MX_X_CUBE_AI_Process\s+App_CameraAi_ProcessHook\s*$')) {
  throw 'main.c does not redirect the generated AI process hook'
}

if ([regex]::IsMatch($mainClean, '(?ms)\b(?:Camera_Buffer|LCD_CopyBuffer|OV5640_DCMI_(?:Suspend|Resume))\b')) {
  throw 'main.c owns camera runtime work'
}

if (-not [regex]::IsMatch($aiClean, '(?ms)^\s*void\s+AI_RequestProcess\s*\(\s*void\s*\)\s*\{') -or
    -not [regex]::IsMatch($aiClean, '(?ms)^\s*int\s+AI_ProcessPendingRequest\s*\(\s*void\s*\)\s*\{')) {
  throw 'AI request gate is missing'
}

Write-Host 'Boundary check passed.'
