$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $repoRoot "windows-receiver"
$buildDir = Join-Path $sourceDir "build"
$config = "Release"

Write-Host "Configuring receiver..."
cmake -S $sourceDir -B $buildDir | Out-Host

Write-Host "Building receiver..."
cmake --build $buildDir --config $config --clean-first | Out-Host

$exePath = Join-Path $buildDir "$config\AndroidCastReceiver.exe"
if (-not (Test-Path $exePath)) {
    throw "Receiver executable not found: $exePath"
}

Write-Host "Launching receiver..."
Start-Process -FilePath $exePath
