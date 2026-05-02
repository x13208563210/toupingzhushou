#ifndef SourceDir
  #error "Missing SourceDir define."
#endif

#ifndef OutputDir
  #error "Missing OutputDir define."
#endif

#ifndef VersionLabel
  #error "Missing VersionLabel define."
#endif

#ifndef AppVersion
  #error "Missing AppVersion define."
#endif

#ifndef BuildDate
  #define BuildDate "unknown"
#endif

#ifndef SetupIconFile
  #error "Missing SetupIconFile define."
#endif

#ifndef AppExeName
  #define AppExeName "直播投屏助手.exe"
#endif

[Setup]
AppId={{B3D0F978-4C9A-4B14-A4D4-881D3B73AC18}
AppName=直播投屏助手
AppVersion={#AppVersion}
AppVerName=直播投屏助手 v{#VersionLabel}
AppPublisher=z6y
VersionInfoCompany=z6y
VersionInfoDescription=直播投屏助手安装程序
VersionInfoProductName=直播投屏助手安装程序
VersionInfoProductVersion={#AppVersion}
VersionInfoTextVersion={#VersionLabel}
DefaultDirName={code:GetDefaultInstallDir}
DefaultGroupName=直播投屏助手
DisableProgramGroupPage=yes
UninstallDisplayName=直播投屏助手
UninstallDisplayIcon={app}\{#AppExeName}
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=直播投屏助手-v{#VersionLabel}-安装程序
SetupIconFile={#SetupIconFile}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=no
CloseApplications=yes
RestartApplications=no
AppMutex=Zhou6YLiveCastAssistantSingleton
UsePreviousAppDir=yes
DirExistsWarning=yes
SetupLogging=yes

[Languages]
Name: "chinesesimp"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\直播投屏助手"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\直播投屏助手"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon
Name: "{autoprograms}\卸载 直播投屏助手"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#AppExeName}"; Description: "立即启动直播投屏助手"; Flags: nowait postinstall skipifsilent

[Code]
function GetDefaultInstallDir(Param: string): string;
begin
  if DirExists('D:\') then
    Result := 'D:\z6y\直播助手'
  else
    Result := ExpandConstant('{autopf}\直播助手');
end;
