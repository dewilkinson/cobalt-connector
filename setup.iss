; Cobalt Worker installer - Inno Setup 6, per user, no administrator rights.
; Built by build.ps1:  ISCC /DStageDir=<stage> /DOutDir=<dist> setup.iss
;
; Unattended install (all parameters optional except the partner key):
;   CobaltWorkerSetup.exe /VERYSILENT /SUPPRESSMSGBOXES /DIR="C:\path" /PARTNERKEYFILE=C:\path\partner.txt
;       [/ANTHROPICKEYFILE=C:\path\anthropic.txt] [/CORES=6] [/MEMORYGB=12] [/HOURS=22:00-07:00]
;       [/RELAY=https://...] [/STARTNOW=0|1] [/RUNATLOGON=0|1] [/SKIPKEYCHECK=1]
; /PARTNERKEY=<key> and /ANTHROPICKEY=... work too, but a key given on the command line is written
; into Setup's log when /LOG is used: prefer the *FILE forms (a file holding just the key).
; A silent install without a partner key stops before installing anything.

#ifndef StageDir
  #define StageDir "stage"
#endif
#ifndef OutDir
  #define OutDir "dist"
#endif
#define AppVersion "1.0.0"
#define AppExe "CobaltWorker.exe"

[Setup]
AppId={{8C1B6F0E-5A57-4E0B-9C1E-C0BA17A0C0E5}
AppName=Cobalt Worker
AppVersion={#AppVersion}
AppVerName=Cobalt Worker {#AppVersion}
AppPublisher=Cobalt
DefaultDirName={localappdata}\Programs\Cobalt Worker
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=CobaltWorkerSetup
SetupIconFile=app\res\cobalt.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName=Cobalt Worker
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
Source: "{#StageDir}\python\*"; DestDir: "{app}\python"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\bootstrap\*"; DestDir: "{app}\bootstrap"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\Cobalt Worker"; Filename: "{app}\{#AppExe}"; Comment: "Use Cobalt with your own Anthropic key, and optionally lend it spare cores"

[UninstallDelete]
; what the app made after install: Python's caches (the downloaded worker build lives under
; %LOCALAPPDATA%\CobaltWorker\build, removed with the rest of that folder below)
Type: filesandordirs; Name: "{app}\bootstrap"
Type: filesandordirs; Name: "{app}\python"
Type: dirifempty; Name: "{app}"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'CobaltWorker';
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

function HomeDir(): String;
begin
  Result := ExpandConstant('{localappdata}\CobaltWorker');
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
    SuppressibleMsgBox('Cobalt Worker does not run without a partner key. Ask whoever runs your fleet for yours.',
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
         'Continue offline? Your key is stored now and checked when the worker first connects; ' +
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
  WizardForm.NextButton.Enabled := Trim(PartnerPage.Values[0]) <> '';
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

procedure InitializeWizard();
var
  Memo: TNewMemo;
  Lbl: TNewStaticText;
  Cores, Mem: Integer;
begin
  CpuCount := StrToIntDef(GetEnv('NUMBER_OF_PROCESSORS'), 4);
  MemoryGB := MachineMemoryGB();
  Cores := CpuCount - 2;
  if Cores < 1 then Cores := 1;
  Mem := MemoryGB - 4;
  if Mem < 4 then Mem := 4;
  if Mem > MemoryGB then Mem := MemoryGB;

  LimitsPage := CreateInputQueryPage(wpSelectDir, 'Your limits',
    'How much of this computer Cobalt Worker may use.',
    'This computer has ' + IntToStr(CpuCount) + ' logical cores and ' + IntToStr(MemoryGB) +
    ' GB of memory. The worker never uses more than you allow here, runs at below-normal priority, ' +
    'and you can change these at any time from its window.');
  LimitsPage.Add('Cores it may use (1 to ' + IntToStr(CpuCount) + '):', False);
  LimitsPage.Add('Memory it may use, in GB (1 to ' + IntToStr(MemoryGB) + '):', False);
  LimitsPage.Add('Hours it may work, e.g. 22:00-07:00 (leave empty for any hour):', False);
  LimitsPage.Values[0] := IntToStr(Cores);
  LimitsPage.Values[1] := IntToStr(Mem);
  LimitsPage.Values[2] := '';
  if Param('CORES') <> '' then LimitsPage.Values[0] := Param('CORES');
  if Param('MEMORYGB') <> '' then LimitsPage.Values[1] := Param('MEMORYGB');
  if Param('HOURS') <> '' then LimitsPage.Values[2] := Param('HOURS');

  PartnerPage := CreateInputQueryPage(LimitsPage.ID, 'Your partner key',
    'Cobalt Worker runs only with a partner key.',
    'Paste the partner key you were given by whoever runs your fleet. It is checked with the Cobalt ' +
    'server when you press Next, then stored encrypted for your Windows user only, and sent only to ' +
    'the Cobalt server.');
  PartnerPage.Add('Partner key:', False);
  PartnerPage.Values[0] := KeyParam('PARTNERKEY');
  PartnerPage.Edits[0].OnChange := @PartnerChanged;

  AnthropicPage := CreateCustomPage(PartnerPage.ID, 'Anthropic API key (optional)',
    'Most people skip this page.');
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
    'Console works.';
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
    'When Cobalt Worker runs.', 'Cobalt Worker sits in the notification area (by the clock) while it runs.',
    False, False);
  StartPage.Add('Start Cobalt Worker now');
  StartPage.Add('Start Cobalt Worker when I sign in to Windows');
  StartPage.Values[0] := ParamFlag('STARTNOW', True);
  StartPage.Values[1] := ParamFlag('RUNATLOGON', True);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if WizardSilent and (KeyParam('PARTNERKEY') = '') then begin
    Log('a silent install needs /PARTNERKEYFILE= or /PARTNERKEY=: nothing was installed');
    SuppressibleMsgBox('A silent install needs /PARTNERKEYFILE=... or /PARTNERKEY=... : Cobalt Worker does not run without a partner key.',
                       mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  WizardForm.NextButton.Caption := SetupMessage(msgButtonNext);
  if CurPageID = PartnerPage.ID then
    WizardForm.NextButton.Enabled := Trim(PartnerPage.Values[0]) <> ''
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
    end;
  end else if CurPageID = PartnerPage.ID then
    Result := CheckPartnerKey();
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Hours: String;
begin
  Hours := Trim(LimitsPage.Values[2]);
  if (Hours = '') or (Lowercase(Hours) = 'any') then Hours := 'any hour';
  Result := MemoDirInfo + NewLine + NewLine +
    'Limits:' + NewLine + Space + Trim(LimitsPage.Values[0]) + ' cores, ' + Trim(LimitsPage.Values[1]) +
    ' GB of memory, ' + Hours + NewLine + NewLine +
    'Partner key:' + NewLine + Space + 'given (stored encrypted)' + NewLine + NewLine +
    'Anthropic key:' + NewLine + Space;
  if Trim(AnthropicEdit.Text) = '' then
    Result := Result + 'skipped'
  else
    Result := Result + 'given (stored encrypted)';
end;

// ------------------------------------------------------------------ installing

procedure StopRunningApp(Exe: String);
var
  Code: Integer;
begin
  if FileExists(Exe) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure WriteLimits();
var
  Hours, Json: String;
begin
  ForceDirectories(HomeDir());
  Hours := Trim(LimitsPage.Values[2]);
  if Hours = '' then Hours := 'any';
  Json := '{' + #13#10 +
    '  "cores": ' + IntToStr(StrToIntDef(Trim(LimitsPage.Values[0]), 1)) + ',' + #13#10 +
    '  "memory_gb": ' + IntToStr(StrToIntDef(Trim(LimitsPage.Values[1]), 4)) + ',' + #13#10 +
    '  "hours": "' + Hours + '",' + #13#10 +
    '  "paused": false,' + #13#10 +
    '  "primed": "' + GetDateTimeString('yyyy-mm-dd"T"hh:nn:ss', '-', ':') + '"' + #13#10 +
    '}';
  SaveStringToFile(HomeDir() + '\limits.json', Json, False);
end;

procedure StoreKeys();
var
  Exe, KeyFile, OutFile, Body: String;
  Code: Integer;
  Msg: AnsiString;
begin
  Exe := ExpandConstant('{app}\{#AppExe}');
  KeyFile := ExpandConstant('{tmp}\keys.txt');
  OutFile := ExpandConstant('{tmp}\keys-result.txt');
  Body := 'partner=' + Trim(PartnerPage.Values[0]) + #13#10 + 'anthropic=' + Trim(AnthropicEdit.Text) + #13#10;
  if Param('RELAY') <> '' then Body := Body + 'relay=' + Param('RELAY') + #13#10;
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
begin
  if CurStep = ssInstall then
    StopRunningApp(ExpandConstant('{app}\{#AppExe}'))
  else if CurStep = ssPostInstall then begin
    WriteLimits();
    StoreKeys();
    if StartPage.Values[1] then
      Exec(ExpandConstant('{app}\{#AppExe}'), '--run-at-logon on', '', SW_HIDE, ewWaitUntilTerminated, Code)
    else
      Exec(ExpandConstant('{app}\{#AppExe}'), '--run-at-logon off', '', SW_HIDE, ewWaitUntilTerminated, Code);
    // started only now, once the keys and the limits are in place
    // (a silent install starts it in the tray only; otherwise its window opens)
    if StartPage.Values[0] then begin
      if WizardSilent then Args := '--background' else Args := '';
      ExecAsOriginalUser(ExpandConstant('{app}\{#AppExe}'), Args, '', SW_SHOWNORMAL, ewNoWait, Code);
    end;
  end;
end;

// ------------------------------------------------------------------ uninstalling

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopRunningApp(ExpandConstant('{app}\{#AppExe}'))
  else if CurUninstallStep = usPostUninstall then begin
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, RunValue);
    // the cache of market data, the logs, the settings and both keys
    DelTree(HomeDir(), True, True, True);
    DelTree(ExpandConstant('{app}'), True, True, True);
  end;
end;
