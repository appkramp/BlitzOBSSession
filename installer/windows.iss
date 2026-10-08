; Inno Setup script for the Windows installer. CI builds it
; (.github/scripts/Package-Windows.ps1); by hand, after
;   cmake --preset windows-x64
;   cmake --build --preset windows-x64 --config Release
;   cmake --install build_x64 --config Release --prefix release\Release
; compile it with Inno Setup 6:
;   iscc /DVersion=1.2.3 /DConfig=Release installer\windows.iss
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
VersionInfoProductName=Blitz Session Stats
VersionInfoProductVersion={#Version}
VersionInfoVersion={#Version}
VersionInfoDescription=Blitz Session Stats installer

[Languages]
; The privacy notice is shown before installing, as SignPath Foundation's
; code signing terms ask of software that talks to a server.
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "privacy-en.txt"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"; InfoBeforeFile: "privacy-ru.txt"
Name: "uk"; MessagesFile: "compiler:Languages\Ukrainian.isl"; InfoBeforeFile: "privacy-uk.txt"

[Files]
Source: "..\release\{#Config}\{#Name}\bin\64bit\{#Name}.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "..\release\{#Config}\{#Name}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs

[InstallDelete]
; An update or a reinstall replaces the plugin's files wholesale, so nothing
; an earlier version shipped is left behind. Settings and the key live in
; %APPDATA%\obs-studio\plugin_config and are kept.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[CustomMessages]
en.CloseObs=Close OBS Studio before installing the plugin.
en.Upgrade=Blitz Session Stats %1 is installed. It will be updated to %2.%n%nSettings and the key are kept.
en.Same=Blitz Session Stats %1 is already installed.%n%nInstall it again, replacing the installed files? Settings and the key are kept.
en.Newer=A newer Blitz Session Stats is installed: %1.%n%nReplace it with %2? Settings and the key are kept.
en.Unknown=Blitz Session Stats is already installed.%n%nReplace it with %1? Settings and the key are kept.
ru.CloseObs=Закройте OBS Studio перед установкой плагина.
ru.Upgrade=Установлен Blitz Session Stats %1. Он будет обновлён до версии %2.%n%nНастройки и ключ сохранятся.
ru.Same=Blitz Session Stats %1 уже установлен.%n%nУстановить его заново, заменив файлы? Настройки и ключ сохранятся.
ru.Newer=Установлена более новая версия Blitz Session Stats: %1.%n%nЗаменить её версией %2? Настройки и ключ сохранятся.
ru.Unknown=Blitz Session Stats уже установлен.%n%nЗаменить его версией %1? Настройки и ключ сохранятся.
uk.CloseObs=Закрийте OBS Studio перед установленням плагіна.
uk.Upgrade=Встановлено Blitz Session Stats %1. Його буде оновлено до версії %2.%n%nНалаштування й ключ збережуться.
uk.Same=Blitz Session Stats %1 уже встановлено.%n%nВстановити його знову, замінивши файли? Налаштування й ключ збережуться.
uk.Newer=Встановлено новішу версію Blitz Session Stats: %1.%n%nЗамінити її версією %2? Налаштування й ключ збережуться.
uk.Unknown=Blitz Session Stats уже встановлено.%n%nЗамінити його версією %1? Налаштування й ключ збережуться.

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B0F1C2E-7F4A-4C8B-9E51-2D6B3A9C4E10}_is1';

// Takes the next dot-separated number off S.
function NextPart(var S: String): Integer;
var
  Dot: Integer;
begin
  Dot := Pos('.', S);
  if Dot = 0 then
  begin
    Result := StrToIntDef(S, 0);
    S := '';
  end
  else
  begin
    Result := StrToIntDef(Copy(S, 1, Dot - 1), 0);
    S := Copy(S, Dot + 1, Length(S));
  end;
end;

// -1, 0 or 1 as A is older than, the same as or newer than B; "1.2.1" and
// "1.2.1.0" are the same.
function CompareVersions(A, B: String): Integer;
var
  I, X, Y: Integer;
begin
  Result := 0;
  for I := 1 to 4 do
  begin
    X := NextPart(A);
    Y := NextPart(B);
    if X < Y then begin Result := -1; Exit; end;
    if X > Y then begin Result := 1; Exit; end;
  end;
end;

// The version installed: what this installer recorded, else the version
// inside the plugin's DLL (a copy installed by hand from the zip). Empty
// when there is none; '?' when there is a plugin of no known version.
function InstalledVersion(): String;
var
  Dll: String;
begin
  if RegQueryStringValue(HKLM64, UninstallKey, 'DisplayVersion', Result) and (Result <> '') then
    Exit;
  if RegQueryStringValue(HKLM32, UninstallKey, 'DisplayVersion', Result) and (Result <> '') then
    Exit;
  Result := '';
  Dll := ExpandConstant('{commonappdata}\obs-studio\plugins\{#Name}\bin\64bit\{#Name}.dll');
  if FileExists(Dll) then
    if not GetVersionNumbersString(Dll, Result) then
      Result := '?';
end;

function InitializeSetup(): Boolean;
var
  Code: Integer;
  Installed: String;
begin
  Result := True;
  // OBS keeps the module loaded while it runs; replacing it then fails.
  if Exec('cmd.exe', '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe"', '', SW_HIDE,
          ewWaitUntilTerminated, Code) and (Code = 0) then
  begin
    MsgBox(CustomMessage('CloseObs'), mbError, MB_OK);
    Result := False;
    Exit;
  end;

  Installed := InstalledVersion();
  if Installed = '' then
    Exit;
  if Installed = '?' then
    Result := MsgBox(FmtMessage(CustomMessage('Unknown'), ['{#Version}']), mbConfirmation, MB_YESNO) = IDYES
  else
    case CompareVersions(Installed, '{#Version}') of
      -1: Result := MsgBox(FmtMessage(CustomMessage('Upgrade'), [Installed, '{#Version}']), mbInformation, MB_OKCANCEL) = IDOK;
      0: Result := MsgBox(FmtMessage(CustomMessage('Same'), ['{#Version}']), mbConfirmation, MB_YESNO) = IDYES;
      1: Result := MsgBox(FmtMessage(CustomMessage('Newer'), [Installed, '{#Version}']), mbConfirmation, MB_YESNO) = IDYES;
    end;
end;
