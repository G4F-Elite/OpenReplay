#define AppName "OpenReplay"
#define AppPublisher "G4F-Elite"
#define AppUrl "https://github.com/G4F-Elite/OpenReplay"

#ifndef AppVersion
  #error AppVersion must be defined by build-installer.ps1
#endif
#ifndef SourceDir
  #error SourceDir must be defined by build-installer.ps1
#endif
#ifndef OutputDir
  #error OutputDir must be defined by build-installer.ps1
#endif

[Setup]
AppId=G4F-Elite.OpenReplay
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases/latest
DefaultDirName={localappdata}\Programs\OpenReplay
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19045
OutputDir={#OutputDir}
OutputBaseFilename=OpenReplay-{#AppVersion}-win-x64-installer
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
AppMutex=Local\OpenReplay.Host.Singleton.v1
UninstallDisplayIcon={app}\app\OpenReplay.App.exe
LicenseFile={#SourceDir}\licenses\OpenReplay-GPL-2.0-or-later.txt
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
Type: filesandordirs; Name: "{app}\app"
Type: filesandordirs; Name: "{app}\app.backup.*"
Type: filesandordirs; Name: "{app}\app.failed.*"
Type: filesandordirs; Name: "{app}\app.staging.*"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}\app"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\OpenReplay"; Filename: "{app}\app\OpenReplay.App.exe"; WorkingDir: "{app}\app"
Name: "{autodesktop}\OpenReplay"; Filename: "{app}\app\OpenReplay.App.exe"; WorkingDir: "{app}\app"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\OpenReplay.App.exe"; ValueType: string; ValueName: ""; ValueData: "{app}\app\OpenReplay.App.exe"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\OpenReplay.App.exe"; ValueType: string; ValueName: "Path"; ValueData: "{app}\app"
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "OpenReplay"; Flags: dontcreatekey uninsdeletevalue

[Run]
Filename: "{app}\app\OpenReplay.App.exe"; WorkingDir: "{app}\app"; Description: "{cm:LaunchProgram,OpenReplay}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\app"
Type: filesandordirs; Name: "{app}\app.backup.*"
Type: filesandordirs; Name: "{app}\app.failed.*"
Type: filesandordirs; Name: "{app}\app.staging.*"
