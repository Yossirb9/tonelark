; Lightspeed installer (Inno Setup 6), based on darktable.iss.in.
; Build it with packaging\windows\make_installer.py, which generates the
; file associations include and passes the install tree:
;   ISCC.exe /DSourceTree=C:\lightspeed\install lightspeed.iss

#ifndef SourceTree
  #define SourceTree "C:\lightspeed\install"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#ifndef DarktableVersion
  #define DarktableVersion "5.6.1"
#endif
#ifndef GphotoVersion
  #define GphotoVersion "2.5.34"
#endif
#ifndef GphotoPortVersion
  #define GphotoPortVersion "0.12.2"
#endif

#define MyAppName "Lightspeed"
#define CurrentYear GetDateTimeString('yyyy', '', '')
#define MyAppCopyright "Copyright (C) 2009-" + CurrentYear + " darktable developers, Lightspeed developers"
#define MyAppPublisher "Lightspeed"
#define MyAppExeName "Lightspeed.exe"
#define MyAppCliExeName "darktable-cli.exe"

[Setup]
AppId=Lightspeed
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppCopyright={#MyAppCopyright}
AppPublisher={#MyAppPublisher}
AppComments=Lightroom-style photo editor built on darktable {#DarktableVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
SourceDir={#SourceTree}
OutputDir={#SourcePath}\..\..\..\dist
OutputBaseFilename=Lightspeed-{#MyAppVersion}-win64-setup
UninstallDisplayIcon={app}\bin\{#MyAppExeName}
SetupIconFile={#SourcePath}\..\..\data\pixmaps\dt_logo_128x128.ico
WizardSmallImageFile={#SourcePath}\..\..\data\pixmaps\256x256\darktable.png
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
ChangesEnvironment=yes
AllowNoIcons=yes
; install for all users by default, or only for the current user without
; administrator rights
PrivilegesRequiredOverridesAllowed=dialog
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=4
WizardStyle=modern
UsedUserAreasWarning=no
CloseApplications=yes
; the photos library and settings live in %LOCALAPPDATA%\lightspeed and are
; never touched by the installer or the uninstaller

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "hebrew"; MessagesFile: "compiler:Languages\Hebrew.isl"

[CustomMessages]
english.ImportFolder=Import folder into Lightspeed
hebrew.ImportFolder=ייבוא התיקייה ל-Lightspeed
english.ImportImage=Import into Lightspeed
hebrew.ImportImage=ייבוא ל-Lightspeed
english.OpenCatalog=Open with Lightspeed (import the catalog)
hebrew.OpenCatalog=פתיחה ב-Lightspeed (ייבוא הקטלוג)
english.AssocCatalog=Open Lightroom catalogs (.lrcat) with Lightspeed
hebrew.AssocCatalog=פתיחת קטלוגים של Lightroom ‏(.lrcat) ב-Lightspeed
english.UserGuide=Lightspeed guide
hebrew.UserGuide=המדריך של Lightspeed

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "lrcatassoc"; Description: "{cm:AssocCatalog}"

[Files]
Source: "bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "share\*"; DestDir: "{app}\share"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "AUTHORS"; DestDir: "{app}"; DestName: "AUTHORS.txt"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[InstallDelete]
; older builds of the same version
Type: filesandordirs; Name: "{app}\lib\darktable\plugins"

[Registry]
; right click on a folder: import it
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\ImportFolderIntoLightspeed"; \
  ValueType: string; ValueData: "{cm:ImportFolder}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\ImportFolderIntoLightspeed"; \
  ValueType: string; ValueName: "Icon"; ValueData: """{app}\bin\{#MyAppExeName}"",0"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\ImportFolderIntoLightspeed\command"; \
  ValueType: string; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey

; right click on a picture: import it
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\ImportImageIntoLightspeed"; \
  ValueType: string; ValueData: "{cm:ImportImage}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\ImportImageIntoLightspeed"; \
  ValueType: string; ValueName: "Icon"; ValueData: """{app}\bin\{#MyAppExeName}"",0"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\ImportImageIntoLightspeed"; \
  ValueType: string; ValueName: "AppliesTo"; ValueData: "System.Kind:=picture"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\ImportImageIntoLightspeed\command"; \
  ValueType: string; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey

; "open with"
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}"; \
  ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#MyAppName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\shell\open\command"; \
  ValueType: string; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey

; Lightroom catalogs
Root: HKA; Subkey: "Software\Classes\Lightspeed.lrcat"; \
  ValueType: string; ValueData: "Lightroom catalog"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lightspeed.lrcat\DefaultIcon"; \
  ValueType: string; ValueData: """{app}\bin\{#MyAppExeName}"",0"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lightspeed.lrcat\shell\open"; \
  ValueType: string; ValueData: "{cm:OpenCatalog}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Lightspeed.lrcat\shell\open\command"; \
  ValueType: string; ValueData: """{app}\bin\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\.lrcat\OpenWithProgids"; \
  ValueType: string; ValueName: "Lightspeed.lrcat"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.lrcat"; \
  ValueType: string; ValueData: "Lightspeed.lrcat"; Flags: uninsdeletevalue; Tasks: lrcatassoc

; raw and image files that this build reads, generated by make_installer.py
#include "lightspeed_openwith.iss"

; launch by name from Win+R and the taskbar search
Root: HKA; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; \
  ValueType: string; ValueData: "{app}\bin\{#MyAppExeName}"; Flags: uninsdeletekey

; tethering: where gphoto2 loads its camera and port drivers from
Root: HKA; Subkey: "{code:EnvironmentKey}"; ValueType: string; ValueName: "CAMLIBS"; \
  ValueData: "{app}\lib\libgphoto2\{#GphotoVersion}"; Flags: uninsdeletevalue
Root: HKA; Subkey: "{code:EnvironmentKey}"; ValueType: string; ValueName: "IOLIBS"; \
  ValueData: "{app}\lib\libgphoto2_port\{#GphotoPortVersion}"; Flags: uninsdeletevalue

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{cm:UserGuide}"; Filename: "{app}\share\doc\lightspeed\guide.html"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\share\doc\lightspeed\guide.html"; Description: "{cm:UserGuide}"; \
  Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\bin\{#MyAppExeName}"; \
  Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; \
  Flags: nowait postinstall skipifsilent

[Code]
function EnvironmentKey(Param: String): String;
begin
  if IsAdminInstallMode then
    Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else
    Result := 'Environment'
end;
