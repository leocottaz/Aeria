; Installateur Windows d'Aeria (Inno Setup 6).
;
;   cmake --install build --prefix stage
;   ISCC /DAppVersion=0.1.0 /DStageDir=..\..\stage /DOutDir=..\..\dist aeria.iss
;
; StageDir = le dossier produit par `cmake --install` : Aeria.exe, ui\, WebView2Loader.dll,
; libwinpthread-1.dll. Installation par utilisateur, sans droits administrateur.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\stage"
#endif
#ifndef OutDir
  #define OutDir "..\..\dist"
#endif

#define AppName "Aeria"
#define AppExe "Aeria.exe"

[Setup]
; Identifiant stable : ne jamais le changer, c'est lui qui relie une mise à jour à l'installation précédente.
AppId={{6F1D2A52-3B7E-4C0B-9A4E-5D1C8E7B2F30}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Aeria
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=Aeria-v{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\..\src\app\aeria.ico
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
; Le dock tourne en arrière-plan : on l'arrête nous-mêmes (voir StopInstalledDock), et seulement la copie
; installée — pas une autre copie lancée depuis un dossier de développement.
CloseApplications=no

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "autostart"; Description: "Lancer {#AppName} à l'ouverture de session"; Flags: unchecked
Name: "desktopicon"; Description: "Créer un raccourci sur le Bureau"; Flags: unchecked

[Files]
Source: "{#StageDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Même valeur que le réglage « Lancer au démarrage » de l'application (src/app/autostart.cpp) :
; elle reste cochée/décochée dans les Réglages, et disparaît à la désinstallation.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Aeria"; ValueData: """{app}\{#AppExe}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "Lancer {#AppName}"; Flags: nowait postinstall skipifsilent
; Mise à jour automatique (features/updater.cpp) : l'application lance ce Setup avec /VERYSILENT /RELAUNCH=1,
; et c'est lui qui la redémarre une fois les fichiers remplacés.
Filename: "{app}\{#AppExe}"; Flags: nowait runasoriginaluser; Check: RelaunchRequested

[Code]
// Arrête le dock installé dans {app} (chemin exact), sans toucher aux autres Aeria.exe.
procedure StopInstalledDock;
var
  Dir: String;
  ResultCode: Integer;
begin
  Dir := ExpandConstant('{app}');
  StringChangeEx(Dir, '''', '''''', True); // apostrophe dans le chemin (ex. nom d'utilisateur)
  // Seulement l'exécutable de {app}.
  Exec('powershell.exe',
    '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Get-Process Aeria -ErrorAction SilentlyContinue' +
    ' | Where-Object { $_.Path -eq ''' + Dir + '\Aeria.exe'' } | Stop-Process -Force"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(300); // laisse Windows libérer les fichiers
end;

function RelaunchRequested: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopInstalledDock; // mise à jour par-dessus une version en cours d'exécution
  Result := '';
end;

// Retire la valeur de démarrage automatique « Name » si elle lance la copie installée ici — pas une autre copie.
procedure RemoveRunValue(Name: String);
var
  Value: String;
begin
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', Name, Value) and
     (Pos(Lowercase(ExpandConstant('{app}')), Lowercase(Value)) > 0) then
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', Name);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopInstalledDock;
  // Démarrage automatique activé dans les Réglages de l'application (et non via la case de l'installateur) :
  // la valeur existe quand même.
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveRunValue('Aeria');
  end;
end;

// Les données de l'utilisateur (rappels, notes, clés API) sont conservées à la désinstallation :
//   %APPDATA%\Aeria et %LOCALAPPDATA%\Aeria
