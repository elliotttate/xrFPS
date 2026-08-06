; xrFPS installer. Built by scripts\Build-Release.ps1, which overrides the
; version defines below; the defaults exist so the script can be bypassed.

#ifndef MyAppVersion
  #define MyAppVersion "0.5.1"
#endif
#ifndef MyFileVersion
  #define MyFileVersion "0.5.1.0"
#endif
#ifndef PayloadRoot
  #define PayloadRoot "..\artifacts\payload"
#endif
#ifndef OutputDirectory
  #define OutputDirectory "..\artifacts"
#endif

#define MyAppName "xrFPS"
#define MyAppExeName "xrFPS.ControlCenter.exe"
#define MyAppPublisher "Elliott Tate"
#define MyAppUrl "https://github.com/elliotttate/xrFPS"

[Setup]
; A fixed AppId is what makes every future installer upgrade this one in
; place rather than installing a second copy alongside it.
AppId={{3F7C2A61-9E42-4B18-8F5D-2C6A1B0D77E4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}/issues
AppUpdatesURL={#MyAppUrl}/releases
VersionInfoVersion={#MyFileVersion}
VersionInfoTextVersion={#MyAppVersion}

; Everything xrFPS touches is per-user (its layer registers under HKCU), so
; setup never needs to elevate.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\xrFPS
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDirectory}
OutputBaseFilename=xrFPS-{#MyAppVersion}-Setup
SetupIconFile=..\app\Assets\xrFPS.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
LicenseFile=..\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Let the Restart Manager close a running Control Center; the app relaunches
; itself through its own watcher, so setup must not also restart it.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "registerlayer"; Description: "Enable the xrFPS overlay for OpenXR games"; GroupDescription: "Setup:"

[Files]
Source: "{#PayloadRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Registering the layer writes to HKCU and copies into %LOCALAPPDATA%, so it
; must run as the invoking user even if setup was elevated.
Filename: "{app}\{#MyAppExeName}"; Parameters: "--install"; StatusMsg: "Registering the xrFPS OpenXR layer..."; Flags: runhidden waituntilterminated runasoriginaluser; Tasks: registerlayer
; skipifsilent matters: the in-app updater runs this installer with
; /VERYSILENT and relaunches the app itself, so letting setup start it too
; would leave two instances running.
Filename: "{app}\{#MyAppExeName}"; Description: "Open the xrFPS Control Center"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterXrFpsLayer"
