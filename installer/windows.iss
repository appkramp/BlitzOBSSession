; Inno Setup script for the Windows installer. CI builds it
; (.github/scripts/Package-Windows.ps1); by hand, after
;   cmake --preset windows-x64
;   cmake --build --preset windows-x64 --config Release
;   cmake --install build_x64 --config Release --prefix release\Release
; compile it with Inno Setup 6:
;   iscc /DVersion=0.2.0 /DConfig=Release installer\windows.iss
;
; OBS 28 and later load plugins from %ProgramData%\obs-studio\plugins\<name>,
; with the module in bin\64bit and the resources in data.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Config
  #define Config "Release"
#endif
#define Name "blitz-session-stats"

[Setup]
AppId={{6B0F1C2E-7F4A-4C8B-9E51-2D6B3A9C4E10}
AppName=Blitz Session Stats
AppVersion={#Version}
AppPublisher=AppKramp
AppPublisherURL=https://stats.appkramp.com
DefaultDirName={commonappdata}\obs-studio\plugins\{#Name}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\release
OutputBaseFilename={#Name}-{#Version}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=Blitz Session Stats (OBS plugin)

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "uk"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Files]
Source: "..\release\{#Config}\{#Name}\bin\64bit\{#Name}.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "..\release\{#Config}\{#Name}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs

[Code]
// OBS keeps the module loaded while it runs; replacing it then fails.
function InitializeSetup(): Boolean;
var
  Code: Integer;
begin
  Result := True;
  if Exec('cmd.exe', '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe"', '', SW_HIDE,
          ewWaitUntilTerminated, Code) and (Code = 0) then
  begin
    MsgBox('Close OBS Studio before installing the plugin.', mbError, MB_OK);
    Result := False;
  end;
end;
