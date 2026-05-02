param(
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Step {
    param([string]$Message)
    Write-Host "==> $Message"
}

function Get-BuildLabel {
    param([string]$AppMainPath)

    $content = Get-Content -LiteralPath $AppMainPath -Raw -Encoding UTF8
    $match = [regex]::Match($content, 'kAppBuildLabel\[\]\s*=\s*L"([^"]+)"')
    if (-not $match.Success) {
        throw "无法从 AppMain.cpp 解析构建标签。"
    }
    return $match.Groups[1].Value
}

function Get-NumericVersion {
    param([string]$RcPath)

    $content = Get-Content -LiteralPath $RcPath -Raw -Encoding UTF8
    $match = [regex]::Match($content, 'FILEVERSION\s+(\d+),(\d+),(\d+),(\d+)')
    if (-not $match.Success) {
        throw "无法从 receiver.rc 解析文件版本。"
    }
    return '{0}.{1}.{2}.{3}' -f $match.Groups[1].Value, $match.Groups[2].Value, $match.Groups[3].Value, $match.Groups[4].Value
}

function Get-MsBuildPath {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $resolved = & $vswherePath -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($resolved) {
            return $resolved
        }
    }

    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw '未找到 MSBuild.exe。'
}

function Get-IsccPath {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $registryPaths = @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($registryPath in $registryPaths) {
        $installLocation = Get-ChildItem -Path $registryPath -ErrorAction SilentlyContinue |
            Get-ItemProperty -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -like '*Inno Setup*' -and $_.InstallLocation } |
            Select-Object -First 1 -ExpandProperty InstallLocation
        if ($installLocation) {
            $candidate = Join-Path $installLocation 'ISCC.exe'
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    throw '未找到 Inno Setup 编译器 ISCC.exe。'
}

function Invoke-ExternalCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory
    )

    $argumentPreview = [string]::Join(' ', $Arguments)
    Write-Step ("运行: {0} {1}" -f $FilePath, $argumentPreview)

    Push-Location -LiteralPath $WorkingDirectory
    try {
        & $FilePath @Arguments
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if ($exitCode -ne 0) {
        throw ("命令执行失败，退出码: {0}" -f $exitCode)
    }
}

function Write-Utf8File {
    param(
        [string]$Path,
        [string]$Content
    )

    Set-Content -LiteralPath $Path -Value $Content -Encoding UTF8
}

function Copy-Directory {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        throw ("缺少目录: {0}" -f $SourcePath)
    }

    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Recurse -Force
}

function Copy-File {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        throw ("缺少文件: {0}" -f $SourcePath)
    }

    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
}

function Try-CopyFile {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    try {
        Copy-File -SourcePath $SourcePath -DestinationPath $DestinationPath
        return $true
    } catch {
        Write-Warning ("同步文件失败，已跳过: {0}" -f $_.Exception.Message)
        return $false
    }
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$receiverRoot = Join-Path $repoRoot 'windows-receiver'
$buildRoot = Join-Path $receiverRoot 'build-v18'
$releaseRoot = Join-Path $buildRoot 'Release'
$deliverableRoot = Join-Path $repoRoot '成品'
$portableRoot = Join-Path $deliverableRoot 'Windows接收端'
$stageRoot = Join-Path $repoRoot 'build\installer-stage\直播投屏助手'
$issPath = Join-Path $PSScriptRoot 'windows-receiver-installer.iss'
$readmeTemplatePath = Join-Path $PSScriptRoot '直播投屏助手使用说明.txt.template'
$autoReplyReadmeTemplatePath = Join-Path $PSScriptRoot '自动回复说明.txt'
$receiverRcPath = Join-Path $receiverRoot 'resources\receiver.rc'
$appMainPath = Join-Path $receiverRoot 'src\AppMain.cpp'
$setupIconPath = Join-Path $receiverRoot 'resources\z6y-logo.ico'
$mainExeSourcePath = Join-Path $releaseRoot 'AndroidCastReceiver.exe'
$installedExeName = '直播投屏助手.exe'
$defaultInstallDir = 'D:\z6y\直播助手'
$installedAppRoot = $defaultInstallDir
$installedAutoReplyConfigPath = Join-Path $installedAppRoot '弹幕自动回复\自动回复配置.json'
$installedDanmakuRegionPath = Join-Path $installedAppRoot '弹幕识别\弹幕区域.json'

$buildLabel = Get-BuildLabel -AppMainPath $appMainPath
$numericVersion = Get-NumericVersion -RcPath $receiverRcPath
$shortVersion = ($buildLabel -split '-')[0]
$buildDate = $buildLabel.Substring($buildLabel.IndexOf('-') + 1)
$portableVersionedExePath = Join-Path $portableRoot ("直播投屏助手-v{0}.exe" -f $buildLabel)

if (-not $SkipBuild) {
    $msbuildPath = Get-MsBuildPath
    Write-Step '重建 Windows 接收端 Release x64'
    Invoke-ExternalCommand -FilePath $msbuildPath -Arguments @(
        (Join-Path $buildRoot 'AndroidCastReceiver.sln'),
        '/t:Rebuild',
        '/m',
        '/p:Configuration=Release;Platform=x64'
    ) -WorkingDirectory $buildRoot
}

if (-not (Test-Path -LiteralPath $mainExeSourcePath)) {
    throw ("编译完成后仍未找到主程序: {0}" -f $mainExeSourcePath)
}

Write-Step '尝试同步主程序到现有裸包目录'
Try-CopyFile -SourcePath $mainExeSourcePath -DestinationPath (Join-Path $portableRoot 'AndroidCastReceiver.exe') | Out-Null
Try-CopyFile -SourcePath $mainExeSourcePath -DestinationPath $portableVersionedExePath | Out-Null

Write-Step '准备安装包暂存目录'
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null

$releaseDirectories = @(
    'voice-model',
    'voice-runtime',
    'web-ui',
    '语音点歌'
)

foreach ($directoryName in $releaseDirectories) {
    Copy-Directory `
        -SourcePath (Join-Path $releaseRoot $directoryName) `
        -DestinationPath (Join-Path $stageRoot $directoryName)
}

$releaseFiles = @(
    'avcodec-61.dll',
    'avutil-59.dll',
    'danmaku-reminder.wav',
    'gift-reminder.wav',
    'live-cast-virtual-camera-media-source.dll',
    'live-cast-virtual-camera-tool.exe',
    'swresample-5.dll',
    'swscale-8.dll',
    'virtual-camera-default.png'
)

foreach ($fileName in $releaseFiles) {
    Copy-File `
        -SourcePath (Join-Path $releaseRoot $fileName) `
        -DestinationPath (Join-Path $stageRoot $fileName)
}

$portableFiles = @(
    'concrt140.dll',
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll',
    'vccorlib140.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll',
    'vcruntime140_threads.dll'
)

foreach ($fileName in $portableFiles) {
    Copy-File `
        -SourcePath (Join-Path $portableRoot $fileName) `
        -DestinationPath (Join-Path $stageRoot $fileName)
}

$airPlaySourcePath = Join-Path $portableRoot 'apple-airplay'
if (-not (Test-Path -LiteralPath $airPlaySourcePath)) {
    $airPlaySourcePath = Join-Path $repoRoot 'tools\uxplay-windows\installed'
}
Copy-Directory -SourcePath $airPlaySourcePath -DestinationPath (Join-Path $stageRoot 'apple-airplay')

Copy-File -SourcePath $mainExeSourcePath -DestinationPath (Join-Path $stageRoot $installedExeName)

$readmeTemplate = Get-Content -LiteralPath $readmeTemplatePath -Raw -Encoding UTF8
$readmeContent = $readmeTemplate.Replace('{{APP_VERSION}}', $shortVersion)
$readmeContent = $readmeContent.Replace('{{BUILD_DATE}}', $buildDate)
$readmeContent = $readmeContent.Replace('{{DEFAULT_INSTALL_DIR}}', $defaultInstallDir)
$readmeContent = $readmeContent.Replace('{{APP_EXE_NAME}}', $installedExeName)
Write-Utf8File -Path (Join-Path $stageRoot '直播投屏助手使用说明.txt') -Content $readmeContent

$autoReplyDir = Join-Path $stageRoot '弹幕自动回复'
New-Item -ItemType Directory -Path $autoReplyDir -Force | Out-Null
Copy-File -SourcePath $autoReplyReadmeTemplatePath -DestinationPath (Join-Path $autoReplyDir '自动回复说明.txt')
$defaultAutoReplyConfig = @'
{
  "enabled": false,
  "sendEnabled": false,
  "cooldownSeconds": 12,
  "delayMs": 1600,
  "inputPointConfigured": false,
  "inputPointXPermille": 375,
  "inputPointYPermille": 959,
  "sendPointConfigured": false,
  "sendPointXPermille": 843,
  "sendPointYPermille": 959,
  "giftEnabled": false,
  "scheduleEnabled": false,
  "scheduleIntervalSeconds": 90,
  "exactRulesText": "",
  "keywordRulesText": "",
  "rulesText": "",
  "fallbackRepliesText": "",
  "giftRepliesText": "",
  "scheduleRepliesText": ""
}
'@
if (Test-Path -LiteralPath $installedAutoReplyConfigPath) {
    Copy-File -SourcePath $installedAutoReplyConfigPath -DestinationPath (Join-Path $autoReplyDir '自动回复配置.json')
    Write-Step '已导入 D 盘现有自动回复配置作为安装默认配置'
} else {
    Write-Utf8File -Path (Join-Path $autoReplyDir '自动回复配置.json') -Content $defaultAutoReplyConfig
}

$danmakuDir = Join-Path $stageRoot '弹幕识别'
New-Item -ItemType Directory -Path $danmakuDir -Force | Out-Null
$defaultDanmakuRegion = @'
{
  "x": 0.000000,
  "y": 0.000000,
  "width": 0.000000,
  "height": 0.000000,
  "valid": false,
  "reminderEnabled": true,
  "giftReminderEnabled": true,
  "speechEnabled": false,
  "speechVoiceIndex": 0
}
'@
if (Test-Path -LiteralPath $installedDanmakuRegionPath) {
    Copy-File -SourcePath $installedDanmakuRegionPath -DestinationPath (Join-Path $danmakuDir '弹幕区域.json')
    Write-Step '已导入 D 盘现有弹幕识别配置作为安装默认配置'
} else {
    Write-Utf8File -Path (Join-Path $danmakuDir '弹幕区域.json') -Content $defaultDanmakuRegion
}

$isccPath = Get-IsccPath
Write-Step '编译中文安装程序'
Invoke-ExternalCommand -FilePath $isccPath -Arguments @(
    "/DSourceDir=$stageRoot",
    "/DOutputDir=$deliverableRoot",
    "/DVersionLabel=$buildLabel",
    "/DAppVersion=$numericVersion",
    "/DBuildDate=$buildDate",
    "/DAppExeName=$installedExeName",
    "/DSetupIconFile=$setupIconPath",
    $issPath
) -WorkingDirectory $PSScriptRoot

$installerPath = Join-Path $deliverableRoot ("直播投屏助手-v{0}-安装程序.exe" -f $buildLabel)
if (-not (Test-Path -LiteralPath $installerPath)) {
    throw ("未找到安装包输出文件: {0}" -f $installerPath)
}

Write-Step '安装包生成完成'
Get-Item -LiteralPath $installerPath | Select-Object FullName, Length, LastWriteTime
