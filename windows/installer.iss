; MultiScreen - installazione Windows (Inno Setup 6)
#define AppVer GetEnv("MS_VERSION")
#if AppVer == ""
  #define AppVer "1.3.0"
#endif

[Setup]
AppId={{6F3C2A51-7E44-4B8C-9C2D-5D1B8E0A9F11}
AppName=MultiScreen
AppVersion={#AppVer}
AppPublisher=Alessandro Zago
AppPublisherURL=https://github.com/Alessandro1234567/multiscreen
DefaultDirName={autopf}\MultiScreen
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=MultiScreen-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=MultiScreen
CloseApplications=force

[Languages]
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "publish\MultiScreen.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\prebuilt\multiscreen.apk"; DestDir: "{app}"; Flags: ignoreversion
Source: "publish\apk-version"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\MultiScreen"; Filename: "{app}\MultiScreen.exe"

[Registry]
; tasto destro su un file -> "Invia al tablet"
Root: HKCR; Subkey: "*\shell\MultiScreen"; ValueType: string; ValueName: ""; ValueData: "Invia al tablet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "*\shell\MultiScreen"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\MultiScreen.exe"
Root: HKCR; Subkey: "*\shell\MultiScreen\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MultiScreen.exe"" --send ""%1"""
Root: HKCR; Subkey: "Directory\shell\MultiScreen"; ValueType: string; ValueName: ""; ValueData: "Invia al tablet"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Directory\shell\MultiScreen\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MultiScreen.exe"" --send ""%1"""

[Run]
Filename: "{app}\MultiScreen.exe"; Parameters: "--setup"; StatusMsg: "Scarico e installo i componenti (driver dello schermo, adb, ffmpeg)... puo' richiedere qualche minuto"; Flags: waituntilterminated
Filename: "schtasks.exe"; Parameters: "/Run /TN MultiScreen"; Flags: runhidden; StatusMsg: "Avvio MultiScreen..."

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/IM MultiScreen.exe /F"; Flags: runhidden; RunOnceId: "kill"
Filename: "taskkill.exe"; Parameters: "/IM ffmpeg.exe /F"; Flags: runhidden; RunOnceId: "killff"
Filename: "schtasks.exe"; Parameters: "/Delete /TN MultiScreen /F"; Flags: runhidden; RunOnceId: "task"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\tools"
