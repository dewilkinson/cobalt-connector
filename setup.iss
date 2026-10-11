; Cobalt Connector installer - Inno Setup 6, per user, no administrator rights.
; Built by build.ps1:  ISCC /DStageDir=<stage> /DOutDir=<dist> /DAppVersion=<version> setup.iss
; (the version comes from app\CMakeLists.txt; the #define below is only a fallback)
;
; Unattended install (all parameters optional except the partner key on a first install):
;   CobaltConnectorSetup.exe /VERYSILENT /SUPPRESSMSGBOXES /DIR="C:\path" /PARTNERKEYFILE=C:\path\partner.txt
;       [/ANTHROPICKEYFILE=C:\path\anthropic.txt] [/CORES=6] [/MEMORYGB=12] [/HOURS=22:00-07:00]
;       [/MACHINENAME="Gaming PC"] [/RELAY=https://...] [/STARTNOW=0|1] [/RUNATLOGON=0|1] [/SKIPKEYCHECK=1]
; /PARTNERKEY=<key> and /ANTHROPICKEY=... work too, but a key given on the command line is written
; into Setup's log when /LOG is used: prefer the *FILE forms (a file holding just the key).
;
; Update mode (/UPDATE, or a silent install on a machine that already holds a partner key and is given
; none): the key pages are skipped, the stored keys are left exactly as they are, and limits.json keeps
; every setting the owner made (only settings it lacks are filled in). The app's self-update runs
;   CobaltConnectorSetup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /UPDATE /STARTNOW=1 /DIR=...
; A silent install with no partner key, on a machine that has none stored, stops before installing anything.
;
; Upgrading from Cobalt Worker (1.1.0 and older): the same AppId, so it replaces that install in place and
; Apps & features keeps one entry. The old app is quit, its folder, shortcut and "run at logon" entry are
; removed, and the app moves the old settings folder (%LOCALAPPDATA%\CobaltWorker) on its first start.

#ifndef StageDir
  #define StageDir "stage"
#endif
#ifndef OutDir
  #define OutDir "dist"
#endif
#ifndef AppVersion
  #define AppVersion "1.3.0"
#endif
#define AppExe "CobaltConnector.exe"
#define LegacyExe "CobaltWorker.exe"

[Setup]
AppId={{8C1B6F0E-5A57-4E0B-9C1E-C0BA17A0C0E5}
AppName=Cobalt Connector
AppVersion={#AppVersion}
AppVerName=Cobalt Connector {#AppVersion}
AppPublisher=Cobalt
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\Cobalt Connector
UsePreviousAppDir=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=CobaltConnectorSetup
SetupIconFile=app\res\cobalt.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName=Cobalt Connector
LicenseFile=licence.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
CloseApplications=no
SetupLogging=yes

[Files]
Source: "{#StageDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
; Python goes in beside the old one and is swapped in at the end (SwapInNewPython), so an install that
; stops halfway (a file held open, a full disk) leaves the earlier version's Python whole and working
Source: "{#StageDir}\python\*"; DestDir: "{app}\python.new"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\bootstrap\*"; DestDir: "{app}\bootstrap"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\Cobalt Connector"; Filename: "{app}\{#AppExe}"; Comment: "Use Cobalt with your own Anthropic key, and optionally lend it spare cores"

[InstallDelete]
; the Start-menu entry of Cobalt Worker (1.1.0 and older)
Type: files; Name: "{userprograms}\Cobalt Worker.lnk"
; what an earlier install that stopped halfway left behind (never the Python in use: see [Files])
Type: filesandordirs; Name: "{app}\python.new"
Type: filesandordirs; Name: "{app}\python.old"

[UninstallDelete]
; what the app made after install: Python's caches (the downloaded worker build lives under
; %LOCALAPPDATA%\CobaltConnector\build, removed with the rest of that folder below)
Type: filesandordirs; Name: "{app}\bootstrap"
Type: filesandordirs; Name: "{app}\python"
Type: filesandordirs; Name: "{app}\python.new"
Type: filesandordirs; Name: "{app}\python.old"
Type: files; Name: "{userprograms}\Cobalt Worker.lnk"
Type: dirifempty; Name: "{app}"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'CobaltConnector';
  LegacyRunValue = 'CobaltWorker';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{8C1B6F0E-5A57-4E0B-9C1E-C0BA17A0C0E5}_is1';
  KeyAccepted = 0;
  KeyRefused = 1;
  KeyUnchecked = 2;

type
  TMemoryStatusEx = record
    dwLength: DWORD;
    dwMemoryLoad: DWORD;
    ullTotalPhys: Int64;
    ullAvailPhys: Int64;
    ullTotalPageFile: Int64;
    ullAvailPageFile: Int64;
    ullTotalVirtual: Int64;
    ullAvailVirtual: Int64;
    ullAvailExtendedVirtual: Int64;
  end;

function GlobalMemoryStatusEx(var lpBuffer: TMemoryStatusEx): BOOL;
  external 'GlobalMemoryStatusEx@kernel32.dll stdcall';

var
  LimitsPage: TInputQueryWizardPage;
  PartnerPage: TInputQueryWizardPage;
  AnthropicPage: TWizardPage;
  AnthropicEdit: TNewEdit;
  StartPage: TInputOptionWizardPage;
  CheckedKey: String;          // the partner key that passed the check (or was let through offline)
  CpuCount, MemoryGB: Integer;
  KeepSettings: Boolean;       // a partner key is already stored and none was given: keep keys and limits
  UpdateMode: Boolean;         // /UPDATE (the app's self-update) or a silent reinstall: no key pages at all
  PreviousDir: String;         // where an earlier version is installed ('' when none)
  StoppedExe: String;          // the app this setup asked to quit ('' until it has)
  Installed: Boolean;          // ssPostInstall ran: the new version is in place and started

function HomeDir(): String;
begin
  Result := ExpandConstant('{localappdata}\CobaltConnector');
end;

function LegacyHomeDir(): String;
begin
  Result := ExpandConstant('{localappdata}\CobaltWorker');
end;

function LegacyAppDir(): String;
begin
  Result := ExpandConstant('{localappdata}\Programs\Cobalt Worker');
end;

function Param(Name: String): String;
begin
  Result := Trim(ExpandConstant('{param:' + Name + '|}'));
end;

function ParamFlag(Name: String; Default: Boolean): Boolean;
var
  V: String;
begin
  V := Lowercase(Param(Name));
  if V = '' then
    Result := Default
  else
    Result := (V = '1') or (V = 'yes') or (V = 'true') or (V = 'on');
end;

// a key from /NAME=... or from the file /NAMEFILE=... names
function KeyParam(Name: String): String;
var
  S: AnsiString;
begin
  Result := Param(Name);
  if (Result = '') and (Param(Name + 'FILE') <> '') then
    if LoadStringFromFile(Param(Name + 'FILE'), S) then
      Result := Trim(String(S));
end;

function RelayArg(): String;
begin
  Result := '';
  if Param('RELAY') <> '' then
    Result := ' --relay "' + Param('RELAY') + '"';
end;

function ExistingPartnerKey(): Boolean;
begin
  Result := FileExists(HomeDir() + '\partner.key') or FileExists(LegacyHomeDir() + '\partner.key');
end;

function MachineMemoryGB(): Integer;
var
  M: TMemoryStatusEx;
begin
  M.dwLength := 64;
  if GlobalMemoryStatusEx(M) then
    Result := M.ullTotalPhys div (1024 * 1024 * 1024)
  else
    Result := 8;
  if Result < 1 then Result := 1;
end;

function ValidHours(H: String): Boolean;
var
  I: Integer;
  A, B, C, D: Integer;
begin
  H := Trim(H);
  if (H = '') or (Lowercase(H) = 'any') then begin
    Result := True;
    Exit;
  end;
  Result := False;
  if Length(H) <> 11 then Exit;
  if (H[3] <> ':') or (H[6] <> '-') or (H[9] <> ':') then Exit;
  for I := 1 to 11 do
    if not ((I = 3) or (I = 6) or (I = 9)) then
      if (H[I] < '0') or (H[I] > '9') then Exit;
  A := StrToInt(Copy(H, 1, 2)); B := StrToInt(Copy(H, 4, 2));
  C := StrToInt(Copy(H, 7, 2)); D := StrToInt(Copy(H, 10, 2));
  Result := (A < 24) and (C < 24) and (B < 60) and (D < 60);
end;

// the name shown to Cobalt: up to 32 letters, digits, spaces, '-', '_' and '.'; empty = other partners see
// "<owner>'s machine N" (the owner and the fleet's admin see the computer's name)
function ValidMachineName(N: String): Boolean;
var
  I: Integer;
  C: Char;
begin
  N := Trim(N);
  Result := Length(N) <= 32;
  for I := 1 to Length(N) do begin
    C := N[I];
    if not (((C >= 'a') and (C <= 'z')) or ((C >= 'A') and (C <= 'Z')) or ((C >= '0') and (C <= '9')) or
            (C = ' ') or (C = '-') or (C = '_') or (C = '.')) then
      Result := False;
  end;
end;

// ------------------------------------------------------------------ the partner key check

function RunCheckKey(Key: String; var Message: String): Integer;
var
  Exe, KeyFile, OutFile: String;
  Code: Integer;
  Msg: AnsiString;
begin
  ExtractTemporaryFile('{#AppExe}');
  Exe := ExpandConstant('{tmp}\{#AppExe}');
  KeyFile := ExpandConstant('{tmp}\check-key.txt');
  OutFile := ExpandConstant('{tmp}\check-key-result.txt');
  DeleteFile(OutFile);
  // the key goes through a file in Setup's own temporary folder, never on a command line
  SaveStringToFile(KeyFile, Key, False);
  if not Exec(Exe, '--check-key-file "' + KeyFile + '" --out "' + OutFile + '"' + RelayArg(), '',
              SW_HIDE, ewWaitUntilTerminated, Code) then begin
    Message := 'The key check could not be run (' + SysErrorMessage(Code) + ').';
    Code := KeyUnchecked;
  end else if LoadStringFromFile(OutFile, Msg) then
    Message := UTF8Decode(Msg)
  else
    Message := 'The key check gave no answer.';
  DeleteFile(KeyFile);
  Result := Code;
end;

function CheckPartnerKey(): Boolean;
var
  Key, Message: String;
  Code: Integer;
begin
  Key := Trim(PartnerPage.Values[0]);
  Result := False;
  if Key = '' then begin
    if KeepSettings then begin
      Result := True;             // the stored key is kept
      Exit;
    end;
    SuppressibleMsgBox('Cobalt Connector does not run without a partner key. Ask whoever runs your fleet for yours.',
                       mbError, MB_OK, IDOK);
    Exit;
  end;
  if Key = CheckedKey then begin
    Result := True;
    Exit;
  end;
  if ParamFlag('SKIPKEYCHECK', False) then begin
    Log('the partner key check was skipped (/SKIPKEYCHECK)');
    CheckedKey := Key;
    Result := True;
    Exit;
  end;
  WizardForm.NextButton.Enabled := False;
  try
    Code := RunCheckKey(Key, Message);
  finally
    WizardForm.NextButton.Enabled := True;
  end;
  Log('partner key check: exit ' + IntToStr(Code));
  if Code = KeyAccepted then begin
    CheckedKey := Key;
    Result := True;
    if not WizardSilent then
      MsgBox(Message, mbInformation, MB_OK);
  end else if Code = KeyRefused then begin
    SuppressibleMsgBox(Message, mbError, MB_OK, IDOK);
  end else begin
    if SuppressibleMsgBox(Message + #13#10#13#10 +
         'Continue offline? Your key is stored now and checked when Cobalt Connector first connects; ' +
         'if it is refused then, the tray icon turns red and says so.',
         mbConfirmation, MB_YESNO, IDYES) = IDYES then begin
      CheckedKey := Key;
      Result := True;
    end;
  end;
end;

// ------------------------------------------------------------------ the pages

procedure PartnerChanged(Sender: TObject);
begin
  WizardForm.NextButton.Enabled := KeepSettings or (Trim(PartnerPage.Values[0]) <> '');
end;

procedure UpdateSkipCaption();
begin
  if Trim(AnthropicEdit.Text) = '' then
    WizardForm.NextButton.Caption := 'Skip'
  else
    WizardForm.NextButton.Caption := SetupMessage(msgButtonNext);
end;

procedure AnthropicChanged(Sender: TObject);
begin
  UpdateSkipCaption();
end;

function RunAtLogonNow(): Boolean;
begin
  Result := RegValueExists(HKEY_CURRENT_USER, RunKey, RunValue) or
            RegValueExists(HKEY_CURRENT_USER, RunKey, LegacyRunValue);
end;

procedure InitializeWizard();
var
  Memo: TNewMemo;
  Lbl: TNewStaticText;
  Cores, Mem: Integer;
  PartnerIntro, AnthropicIntro: String;
begin
  CpuCount := StrToIntDef(GetEnv('NUMBER_OF_PROCESSORS'), 4);
  MemoryGB := MachineMemoryGB();
  Cores := CpuCount - 2;
  if Cores < 1 then Cores := 1;
  Mem := MemoryGB - 4;
  if Mem < 4 then Mem := 4;
  if Mem > MemoryGB then Mem := MemoryGB;

  LimitsPage := CreateInputQueryPage(wpSelectDir, 'Your limits',
    'How much of this computer Cobalt Connector may use, and its name.',
    'This computer has ' + IntToStr(CpuCount) + ' logical cores and ' + IntToStr(MemoryGB) +
    ' GB of memory. Cobalt Connector never uses more than you allow here, runs at below-normal priority, ' +
    'and you can change these at any time from its window.');
  LimitsPage.Add('Cores it may use (1 to ' + IntToStr(CpuCount) + '):', False);
  LimitsPage.Add('Memory it may use, in GB (1 to ' + IntToStr(MemoryGB) + '):', False);
  LimitsPage.Add('Hours it may work, e.g. 22:00-07:00 (leave empty for any hour):', False);
  LimitsPage.Add('Name Cobalt shows for this machine (optional; if empty, other partners see "<your name>''s machine 1"):', False);
  LimitsPage.Values[0] := IntToStr(Cores);
  LimitsPage.Values[1] := IntToStr(Mem);
  LimitsPage.Values[2] := '';
  LimitsPage.Values[3] := '';
  if Param('CORES') <> '' then LimitsPage.Values[0] := Param('CORES');
  if Param('MEMORYGB') <> '' then LimitsPage.Values[1] := Param('MEMORYGB');
  if Param('HOURS') <> '' then LimitsPage.Values[2] := Param('HOURS');
  if Param('MACHINENAME') <> '' then LimitsPage.Values[3] := Param('MACHINENAME');

  if KeepSettings then begin
    PartnerIntro := 'A partner key is already stored on this computer. Leave the box empty to keep it, or ' +
      'paste a new one: a new key is checked with the Cobalt server when you press Next, then stored ' +
      'encrypted for your Windows user only, and sent only to the Cobalt server.';
    AnthropicIntro := 'Leave the box empty to keep things as they are.';
  end else begin
    PartnerIntro := 'Paste the partner key you were given by whoever runs your fleet. It is checked with the ' +
      'Cobalt server when you press Next, then stored encrypted for your Windows user only, and sent only to ' +
      'the Cobalt server.';
    AnthropicIntro := 'Most people skip this page.';
  end;
  PartnerPage := CreateInputQueryPage(LimitsPage.ID, 'Your partner key',
    'Cobalt Connector runs only with a partner key.', PartnerIntro);
  PartnerPage.Add('Partner key:', False);
  PartnerPage.Values[0] := KeyParam('PARTNERKEY');
  PartnerPage.Edits[0].OnChange := @PartnerChanged;

  AnthropicPage := CreateCustomPage(PartnerPage.ID, 'Anthropic API key (optional)', AnthropicIntro);
  Memo := TNewMemo.Create(AnthropicPage);
  Memo.Parent := AnthropicPage.Surface;
  Memo.Left := 0;
  Memo.Top := 0;
  Memo.Width := AnthropicPage.SurfaceWidth;
  Memo.Height := ScaleY(150);
  Memo.ReadOnly := True;
  Memo.ScrollBars := ssVertical;
  Memo.WordWrap := True;
  Memo.Text :=
    'You do not need an Anthropic key to contribute cores. Leave the box empty and press Skip.' + #13#10#13#10 +
    'If you give one:' + #13#10 +
    '- It is used only for YOUR OWN research questions and note replies, never for the fleet owner''s work.' + #13#10 +
    '- Everything it is used for is billed to YOUR Anthropic account.' + #13#10 +
    '- It is stored encrypted for your Windows user only (Windows DPAPI) and sent only to Anthropic.' + #13#10 +
    '- Create a key just for Cobalt in your Anthropic Console (console.anthropic.com) and give it a ' +
    'monthly spend limit there.' + #13#10 +
    '- A Claude chat subscription (Claude Pro or Max) cannot be used here: only an API key from the ' +
    'Console works.' + #13#10 +
    '- You can change or remove it later in the Cobalt Connector window.';
  Lbl := TNewStaticText.Create(AnthropicPage);
  Lbl.Parent := AnthropicPage.Surface;
  Lbl.Top := Memo.Top + Memo.Height + ScaleY(12);
  Lbl.Caption := 'Anthropic API key (optional - leave empty to skip):';
  AnthropicEdit := TNewEdit.Create(AnthropicPage);
  AnthropicEdit.Parent := AnthropicPage.Surface;
  AnthropicEdit.Top := Lbl.Top + Lbl.Height + ScaleY(4);
  AnthropicEdit.Width := AnthropicPage.SurfaceWidth;
  AnthropicEdit.PasswordChar := '*';
  AnthropicEdit.Text := KeyParam('ANTHROPICKEY');
  AnthropicEdit.OnChange := @AnthropicChanged;

  StartPage := CreateInputOptionPage(AnthropicPage.ID, 'Starting',
    'When Cobalt Connector runs.', 'Cobalt Connector sits in the notification area (by the clock) while it runs.',
    False, False);
  StartPage.Add('Start Cobalt Connector now');
  StartPage.Add('Start Cobalt Connector when I sign in to Windows');
  StartPage.Values[0] := ParamFlag('STARTNOW', True);
  if KeepSettings then
    StartPage.Values[1] := ParamFlag('RUNATLOGON', RunAtLogonNow())
  else
    StartPage.Values[1] := ParamFlag('RUNATLOGON', True);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  KeepSettings := ExistingPartnerKey() and (KeyParam('PARTNERKEY') = '');
  UpdateMode := ParamFlag('UPDATE', False) or (WizardSilent and KeepSettings);
  if WizardSilent and (KeyParam('PARTNERKEY') = '') and not ExistingPartnerKey() then begin
    Log('a silent install needs /PARTNERKEYFILE= or /PARTNERKEY=: nothing was installed');
    SuppressibleMsgBox('A silent install needs /PARTNERKEYFILE=... or /PARTNERKEY=... : Cobalt Connector does not run without a partner key.',
                       mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  if UpdateMode then
    Log('update mode: the stored keys and the owner''s settings are kept');
  PreviousDir := '';
  if RegQueryStringValue(HKEY_CURRENT_USER, UninstallKey, 'InstallLocation', PreviousDir) then
    PreviousDir := RemoveBackslashUnlessRoot(PreviousDir);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = LimitsPage.ID then
    Result := KeepSettings or UpdateMode
  else if (PageID = PartnerPage.ID) or (PageID = AnthropicPage.ID) or (PageID = StartPage.ID) then
    Result := UpdateMode;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  WizardForm.NextButton.Caption := SetupMessage(msgButtonNext);
  if CurPageID = PartnerPage.ID then
    WizardForm.NextButton.Enabled := KeepSettings or (Trim(PartnerPage.Values[0]) <> '')
  else if CurPageID = AnthropicPage.ID then
    UpdateSkipCaption()
  else if CurPageID = wpReady then
    WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  N: Integer;
begin
  Result := True;
  if CurPageID = LimitsPage.ID then begin
    N := StrToIntDef(Trim(LimitsPage.Values[0]), 0);
    if (N < 1) or (N > CpuCount) then begin
      SuppressibleMsgBox('Cores must be a whole number from 1 to ' + IntToStr(CpuCount) + '.', mbError, MB_OK, IDOK);
      Result := False;
      Exit;
    end;
    N := StrToIntDef(Trim(LimitsPage.Values[1]), 0);
    if (N < 1) or (N > MemoryGB) then begin
      SuppressibleMsgBox('Memory must be a whole number of GB from 1 to ' + IntToStr(MemoryGB) + '.', mbError, MB_OK, IDOK);
      Result := False;
      Exit;
    end;
    if not ValidHours(LimitsPage.Values[2]) then begin
      SuppressibleMsgBox('Hours must look like 22:00-07:00, or be left empty for any hour.', mbError, MB_OK, IDOK);
      Result := False;
      Exit;
    end;
    if not ValidMachineName(LimitsPage.Values[3]) then begin
      SuppressibleMsgBox('A machine name can be up to 32 characters: letters, digits, spaces, - _ and . only.',
                         mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = PartnerPage.ID then
    Result := CheckPartnerKey();
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Hours, Name: String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if KeepSettings or UpdateMode then
    Result := Result + 'Limits:' + NewLine + Space + 'kept as you set them' + NewLine + NewLine
  else begin
    Hours := Trim(LimitsPage.Values[2]);
    if (Hours = '') or (Lowercase(Hours) = 'any') then Hours := 'any hour';
    Name := Trim(LimitsPage.Values[3]);
    if Name = '' then Name := 'none (other partners see "<your name>''s machine 1")';
    Result := Result + 'Limits:' + NewLine + Space + Trim(LimitsPage.Values[0]) + ' cores, ' +
      Trim(LimitsPage.Values[1]) + ' GB of memory, ' + Hours + NewLine + NewLine +
      'Machine name:' + NewLine + Space + Name + NewLine + NewLine;
  end;
  Result := Result + 'Partner key:' + NewLine + Space;
  if Trim(PartnerPage.Values[0]) <> '' then
    Result := Result + 'given (stored encrypted)'
  else
    Result := Result + 'the stored key is kept';
  Result := Result + NewLine + NewLine + 'Anthropic key:' + NewLine + Space;
  if Trim(AnthropicEdit.Text) <> '' then
    Result := Result + 'given (stored encrypted)'
  else if KeepSettings then
    Result := Result + 'kept as it is'
  else
    Result := Result + 'skipped';
end;

// ------------------------------------------------------------------ installing

procedure StopRunningApp(Exe: String);
var
  Code: Integer;
begin
  if FileExists(Exe) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// before any file is replaced: quit this version, an older one in another folder, and Cobalt Worker
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  StoppedExe := ExpandConstant('{app}\{#AppExe}');
  StopRunningApp(StoppedExe);
  if PreviousDir <> '' then begin
    StopRunningApp(PreviousDir + '\{#AppExe}');
    StopRunningApp(PreviousDir + '\{#LegacyExe}');
  end;
  StopRunningApp(LegacyAppDir() + '\{#LegacyExe}');
end;

// removes what an earlier version installed in a folder other than this one: only its own files, so a
// folder the owner chose that holds other things is left with those things
procedure RemoveOldInstall(Dir: String);
begin
  if (Dir = '') or (CompareText(Dir, ExpandConstant('{app}')) = 0) or not DirExists(Dir) then Exit;
  Log('removing the earlier install in ' + Dir);
  DeleteFile(Dir + '\{#AppExe}');
  DeleteFile(Dir + '\{#LegacyExe}');
  DelTree(Dir + '\python', True, True, True);
  DelTree(Dir + '\python.new', True, True, True);
  DelTree(Dir + '\python.old', True, True, True);
  DelTree(Dir + '\bootstrap', True, True, True);
  DeleteFile(Dir + '\unins000.exe');
  DeleteFile(Dir + '\unins000.dat');
  RemoveDir(Dir);
end;

// The new Python (installed to {app}\python.new) replaces the old one whole, so no stale library file stays
// beside the new ones. The old one is renamed away first and put back if the new one cannot take its place.
procedure SwapInNewPython();
var
  Dir: String;
  I: Integer;
begin
  Dir := ExpandConstant('{app}');
  if not DirExists(Dir + '\python.new') then Exit;
  DelTree(Dir + '\python.old', True, True, True);
  if DirExists(Dir + '\python') then begin
    // the old loop's python.exe may take a moment to exit after --quit
    for I := 1 to 20 do begin
      if RenameFile(Dir + '\python', Dir + '\python.old') then Break;
      Sleep(500);
    end;
    if DirExists(Dir + '\python') then begin
      Log('the old Python is still in use: the new one is left in python.new and the old one kept');
      Exit;
    end;
  end;
  if RenameFile(Dir + '\python.new', Dir + '\python') then
    DelTree(Dir + '\python.old', True, True, True)
  else begin
    Log('the new Python could not be moved into place: the old one is put back');
    RenameFile(Dir + '\python.old', Dir + '\python');
  end;
end;

// limits.json is written by the app (--set-limits), which keeps every setting it is not given. A line
// starting with ? is written only when limits.json does not have that setting yet.
procedure WriteLimits();
var
  Exe, LimitsFile, OutFile, Hours, Body, Keep: String;
  Code: Integer;
begin
  Exe := ExpandConstant('{app}\{#AppExe}');
  LimitsFile := ExpandConstant('{tmp}\limits.txt');
  OutFile := ExpandConstant('{tmp}\limits-result.txt');
  Hours := Trim(LimitsPage.Values[2]);
  if Hours = '' then Hours := 'any';
  if KeepSettings or UpdateMode then Keep := '?' else Keep := '';
  Body :=
    Keep + 'cores=' + IntToStr(StrToIntDef(Trim(LimitsPage.Values[0]), 1)) + #13#10 +
    Keep + 'memory_gb=' + IntToStr(StrToIntDef(Trim(LimitsPage.Values[1]), 4)) + #13#10 +
    Keep + 'hours=' + Hours + #13#10 +
    Keep + 'paused=false' + #13#10 +
    '?updates=auto' + #13#10 +
    '?primed=' + GetDateTimeString('yyyy-mm-dd"T"hh:nn:ss', '-', ':') + #13#10;
  // a name given on the command line is always written; the page's name only on a full install
  if Param('MACHINENAME') <> '' then
    Body := Body + 'name=' + Param('MACHINENAME') + #13#10
  else if (Keep = '') and (Trim(LimitsPage.Values[3]) <> '') then
    Body := Body + 'name=' + Trim(LimitsPage.Values[3]) + #13#10;
  SaveStringToFile(LimitsFile, Body, False);
  if not Exec(Exe, '--set-limits "' + LimitsFile + '" --out "' + OutFile + '"', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  DeleteFile(LimitsFile);
  Log('limits written: exit ' + IntToStr(Code));
end;

procedure StoreKeys();
var
  Exe, KeyFile, OutFile, Body, Partner, Anthropic: String;
  Code: Integer;
  Msg: AnsiString;
begin
  if UpdateMode then begin
    Log('update mode: the stored keys are left as they are');
    Exit;
  end;
  Partner := Trim(PartnerPage.Values[0]);
  Anthropic := Trim(AnthropicEdit.Text);
  Body := '';
  if Partner <> '' then Body := Body + 'partner=' + Partner + #13#10;
  // on a reinstall an empty box keeps the stored Anthropic key; on a first install there is none to keep
  if (Anthropic <> '') or not KeepSettings then Body := Body + 'anthropic=' + Anthropic + #13#10;
  if Param('RELAY') <> '' then Body := Body + 'relay=' + Param('RELAY') + #13#10;
  Partner := '';
  Anthropic := '';
  if Body = '' then Exit;
  Exe := ExpandConstant('{app}\{#AppExe}');
  KeyFile := ExpandConstant('{tmp}\keys.txt');
  OutFile := ExpandConstant('{tmp}\keys-result.txt');
  SaveStringToFile(KeyFile, Body, False);
  Body := '';
  if not Exec(Exe, '--set-keys "' + KeyFile + '" --out "' + OutFile + '"', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  DeleteFile(KeyFile);
  Log('keys stored: exit ' + IntToStr(Code));
  if Code <> 0 then begin
    if not LoadStringFromFile(OutFile, Msg) then Msg := 'The keys could not be stored.';
    SuppressibleMsgBox(UTF8Decode(Msg), mbError, MB_OK, IDOK);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Args: String;
  AtLogon, StartNow: Boolean;
begin
  if CurStep = ssPostInstall then begin
    if UpdateMode then begin
      AtLogon := ParamFlag('RUNATLOGON', RunAtLogonNow());
      StartNow := ParamFlag('STARTNOW', True);
    end else begin
      AtLogon := StartPage.Values[1];
      StartNow := StartPage.Values[0];
    end;
    SwapInNewPython();
    WriteLimits();
    StoreKeys();
    // Cobalt Worker's leftovers: its "run at logon" entry and program folder (the app itself moves the old
    // settings folder on its first start, as it holds the logic for the keys)
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, LegacyRunValue);
    RemoveOldInstall(LegacyAppDir());
    RemoveOldInstall(PreviousDir);
    if AtLogon then
      Exec(ExpandConstant('{app}\{#AppExe}'), '--run-at-logon on', '', SW_HIDE, ewWaitUntilTerminated, Code)
    else
      Exec(ExpandConstant('{app}\{#AppExe}'), '--run-at-logon off', '', SW_HIDE, ewWaitUntilTerminated, Code);
    // started only now, once the keys and the limits are in place
    // (a silent install starts it in the tray only; otherwise its window opens)
    if StartNow then begin
      if WizardSilent then Args := '--background' else Args := '';
      ExecAsOriginalUser(ExpandConstant('{app}\{#AppExe}'), Args, '', SW_SHOWNORMAL, ewNoWait, Code);
    end;
    Installed := True;
  end;
end;

// A self-update (or a silent reinstall) that stopped before the end: the app quit to let it run, so start the
// version that is still there again. On its start it sees the update did not finish and says so.
procedure DeinitializeSetup();
var
  Exe: String;
  Code: Integer;
begin
  if not UpdateMode or Installed then Exit;
  Exe := StoppedExe;
  if Exe = '' then Exe := Param('DIR') + '\{#AppExe}';     // stopped before PrepareToInstall
  if (Param('DIR') = '') and (StoppedExe = '') then Exit;
  if not FileExists(Exe) then begin
    Log('the update did not finish and ' + Exe + ' is not there to start again');
    Exit;
  end;
  Log('the update did not finish: starting ' + Exe + ' again');
  StopRunningApp(Exe);                                    // in case it has not quit yet: one copy only
  ExecAsOriginalUser(Exe, '--background', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

// ------------------------------------------------------------------ uninstalling

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    StopRunningApp(ExpandConstant('{app}\{#AppExe}'));
    StopRunningApp(LegacyAppDir() + '\{#LegacyExe}');
    // this machine's sign-in is retired on the Cobalt server (best effort, 5 s) and its device key deleted from
    // Windows' key store; the pass in token.json goes with the settings folder below
    if FileExists(ExpandConstant('{app}\{#AppExe}')) then
      Exec(ExpandConstant('{app}\{#AppExe}'), '--retire', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end else if CurUninstallStep = usPostUninstall then begin
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, RunValue);
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, LegacyRunValue);
    // the cache of market data, the logs, the settings and both keys (and Cobalt Worker's folder, if left)
    DelTree(HomeDir(), True, True, True);
    DelTree(LegacyHomeDir(), True, True, True);
    DelTree(ExpandConstant('{app}'), True, True, True);
  end;
end;
