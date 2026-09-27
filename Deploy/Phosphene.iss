; Phosphene -- the Windows installer.
;
; Built by Deploy\build_release.ps1, which stages everything under Deploy\stage first and only then
; calls the compiler. Nothing in here reaches into a build tree: what is in the staging folder is
; exactly what gets installed, so the payload can be looked at (and checked, see
; Tools\release\check_package.ps1) before the setup is made.
;
;   ISCC.exe /DVersion=1.0.0 Deploy\Phosphene.iss
;
; The binaries are linked against the static MSVC runtime (PHOS_STATIC_RUNTIME=ON), so there is no
; redistributable to chase and no DLL beside the executable.
;
; WHERE THE FILES GO, AND WHY THERE
;
; The engine opens three data files by bare name -- library.phoswt (the wavetable pack),
; melody.phosmdl and bass.phosmdl (the Phase 8 models). It looks for each of them, in order, as the
; name relative to the working directory, then in the directory a host declared, then in
; PHOS_SOURCE_DATA_DIR, which only ever names a source tree and is therefore of no use to anybody
; who installed this. The declared directory is the one that matters, and each artefact declares a
; different one:
;
;   Standalone / phos_render   the directory the executable is in  -> {app}
;   VST3                       Contents\Resources inside the bundle -> {autocf}\VST3\Phosphene.vst3\Contents\Resources
;
; so all three files are installed twice, once per place. A second copy is cheaper than a plug-in that
; silently plays the six built-in wavetables and the Markov model because a host copied the bundle
; somewhere without the folder next to it.
;
; THE DATA IS DOWNLOADED (23.09.2026, at the user's word: a slim setup, as Noctuary's). The data files are
; not in this setup; Deploy\build_release.ps1 packs them into Phosphene-data-<version>.zip, attached to the
; release, and writes data-files.iss with the archive's hash and every file's own hash. At the "Ready"
; page the [Code] section below decides:
;   - every data file already in {app} with the right hash (an update that changed no data, a reinstall):
;     nothing is downloaded, the files stay, and the VST3 gets copies of them;
;   - the archive lies beside the setup (an offline install): it is taken from there;
;   - otherwise it is downloaded, and Inno checks it against its SHA-256 before unpacking.
; A download that fails does not cost the installation: the question is "install without the data?",
; and without it the engine falls back to its six built-in wavetables and the Markov composer and says
; so on the Set tab.
;
; THE FIELD RECORDINGS (27.09.2026) are a component of their own, on by default: 1.97 GB of original FLAC files
; (Tools\field_select.py), in two archives of a release of their own (field-data-<set>, Tools\field_archives.py),
; so a new version of the program does not upload them again. They are unpacked once, into
; {autoappdata}\Phosphene\field -- ProgramData for a machine-wide install, AppData for "just for me" --, where
; the standalone and the plug-in both look (FieldLibrary.cpp), instead of twice as the data above. Recognised by
; the hash of field\CREDITS-field.md: an installed set is not downloaded again. Without them the program works
; as before and the Field track and the NASA shots are silent (the user: "achte darauf, dass das Programm auch
; dann funktioniert, wenn die Samples nicht heruntergeladen wurden").

#ifndef Version
  #define Version "1.2.0"
#endif
#define AppName "Phosphene"
#define Publisher "Rene Weller"
#define AppURL "https://github.com/reneweller-coding/Phosphene"
#define Stage "stage"
#ifndef DataBaseUrl
  ; Set by Deploy\build_release.ps1 (/DDataBaseUrl=...): the release this setup belongs to.
  #define DataBaseUrl "https://github.com/reneweller-coding/Phosphene/releases/download/v" + Version
#endif
#include "data-files.iss"
; The Field track's recordings (27.09.2026): a release of their own, written by Tools\field_archives.py.
#include "field-files.iss"

[Setup]
AppId={{2F6A4D91-8C13-47B5-A0E2-5D7C9B814E33}
AppName={#AppName}
AppVersion={#Version}
AppVerName={#AppName} {#Version}
AppPublisher={#Publisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#Stage}\LICENSE.txt
OutputDir=out
OutputBaseFilename={#AppName}-{#Version}-Setup
SetupIconFile={#Stage}\phosphene.ico
UninstallDisplayIcon={app}\Phosphene.exe
UninstallDisplayName={#AppName} {#Version}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic
; Machine-wide by default, because the VST3 belongs in the shared plug-in folder and that needs
; administrator rights. Anyone without them picks "just for me" in the first dialog (or passes
; /CURRENTUSER) and gets the per-user plug-in folder; every path below is an {auto...} one, which is
; what lets one script serve both modes.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
VersionInfoVersion={#Version}
VersionInfoCompany={#Publisher}
VersionInfoDescription={#AppName} installer
; The data archive is unpacked by Inno itself (Flags: extractarchive below).
ArchiveExtraction=full

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
en.CompStandalone=Standalone application
en.CompVst3=VST3 plug-in (for a DAW)
en.CompRender=Offline renderer (phos_render, command line)
en.CompManual=Manual (PDF)
en.CompQuest=Meta Quest app (APK, installed with adb)
en.TaskDesktop=Create a desktop shortcut
en.CompData=Wavetables, learned models and voices (downloaded if not installed yet)
en.CompField=Field recordings for the Field track, {#FieldSizeMB} MB (downloaded if not installed yet)
en.FieldDownloadFailed=The field recordings could not be downloaded:%n%n%1%n%nInstall Phosphene without them? Everything plays; only the Field track and the NASA shots stay silent. Running this setup again later fetches them.
en.DownloadFailed=The data could not be downloaded:%n%n%1%n%nInstall Phosphene without it? It then plays with its six built-in wavetables and the Markov composer; running this setup again later fetches the data.
en.NoAvx2=This processor reports no AVX2 support.%n%nPhosphene is built for AVX2, which every x86-64 processor since 2013 has. Without it, it will not start.%n%nInstall anyway?
de.CompStandalone=Eigenstaendiges Programm
de.CompVst3=VST3-Plugin (fuer eine DAW)
de.CompRender=Offline-Renderer (phos_render, Kommandozeile)
de.CompManual=Handbuch (PDF)
de.CompQuest=Meta-Quest-App (APK, wird mit adb installiert)
de.TaskDesktop=Verknuepfung auf dem Desktop anlegen
de.CompData=Wavetables, gelernte Modelle und Stimmen (werden geladen, falls noch nicht installiert)
de.CompField=Field Recordings fuer die Field-Spur, {#FieldSizeMB} MB (werden geladen, falls noch nicht installiert)
de.FieldDownloadFailed=Die Field Recordings konnten nicht geladen werden:%n%n%1%n%nPhosphene ohne sie installieren? Alles spielt; nur die Field-Spur und die NASA-Shots bleiben still. Ein spaeterer Lauf dieses Setups holt sie nach.
de.DownloadFailed=Die Daten konnten nicht geladen werden:%n%n%1%n%nPhosphene ohne sie installieren? Es spielt dann mit seinen sechs eingebauten Wavetables und dem Markov-Komponisten; ein spaeterer Lauf dieses Setups holt die Daten nach.
de.NoAvx2=Dieser Prozessor meldet keine AVX2-Unterstuetzung.%n%nPhosphene ist fuer AVX2 gebaut, das jeder x86-64-Prozessor seit 2013 hat. Ohne AVX2 startet es nicht.%n%nTrotzdem installieren?

[Types]
Name: "full"; Description: "{code:FullTypeName}"
Name: "custom"; Description: "{code:CustomTypeName}"; Flags: iscustom

[Components]
Name: "standalone"; Description: "{cm:CompStandalone}"; Types: full custom; Flags: fixed
Name: "vst3";       Description: "{cm:CompVst3}";       Types: full custom
Name: "render";     Description: "{cm:CompRender}";     Types: full custom
Name: "manual";     Description: "{cm:CompManual}";     Types: full custom
Name: "quest";      Description: "{cm:CompQuest}";      Types: full custom
Name: "data";       Description: "{cm:CompData}";       Types: full custom
Name: "field";      Description: "{cm:CompField}";      Types: full custom; ExtraDiskSpaceRequired: 2000000000

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; Components: standalone

[InstallDelete]
; The lesson Noctuary paid for: it had only [UninstallDelete], so an update installed the new files
; over the old ones and everything that had been RENAMED or DROPPED since stayed behind for ever --
; a stale library.phoswt beside a new binary is not an error anybody sees, it is a wrong sound.
; These lines run before a single file is copied, and they name only what this installer itself
; puts there. Nothing under the user's own Documents is touched, and {app} is not cleared wholesale:
; somebody may keep their .phosset files and their renders next to the program.
Type: filesandordirs; Name: "{autocf}\VST3\Phosphene.vst3"
Type: files;          Name: "{app}\Phosphene-Manual.pdf"
Type: files;          Name: "{app}\PhospheneQuest.apk"
; The data only when new data is coming: when the installed files are already the right ones they stay
; (DataReused), and the VST3 takes its copies from them.
Type: files;          Name: "{app}\library.phoswt";         Check: DataReplaced
Type: files;          Name: "{app}\*.phosmdl";              Check: DataReplaced
Type: files;          Name: "{app}\CREDITS-wavetables.md";  Check: DataReplaced
Type: files;          Name: "{app}\voices.phosvx";          Check: DataReplaced
Type: files;          Name: "{app}\CREDITS-voices.md";      Check: DataReplaced

[Files]
Source: "{#Stage}\Phosphene.exe";  DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt";    DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\README.txt";     DestDir: "{app}"; Components: standalone; Flags: ignoreversion isreadme
Source: "{#Stage}\phosphene.ico";  DestDir: "{app}"; Components: standalone; Flags: ignoreversion
; The data the engine opens by name, beside the binaries that declare {app} as their search path, and in the
; VST3's Contents\Resources: unpacked from the downloaded (or found) archive, or -- when the installed files are
; already the right ones -- copied from {app} into the bundle.
Source: "{tmp}\{#DataZip}"; DestDir: "{app}"; Components: data; Check: DataFetched; \
    Flags: external extractarchive recursesubdirs ignoreversion
Source: "{tmp}\{#DataZip}"; DestDir: "{autocf}\VST3\Phosphene.vst3\Contents\Resources"; Components: data and vst3; Check: DataFetched; \
    Flags: external extractarchive recursesubdirs ignoreversion
Source: "{app}\library.phoswt"; DestDir: "{autocf}\VST3\Phosphene.vst3\Contents\Resources"; Components: data and vst3; Check: DataReused; Flags: external ignoreversion
; The field recordings: both archives into one folder (their paths start with field\).
Source: "{tmp}\{#FieldZipA}"; DestDir: "{autoappdata}\Phosphene"; Components: field; Check: FieldFetched; \
    Flags: external extractarchive recursesubdirs ignoreversion
Source: "{tmp}\{#FieldZipB}"; DestDir: "{autoappdata}\Phosphene"; Components: field; Check: FieldFetched; \
    Flags: external extractarchive recursesubdirs ignoreversion
Source: "{app}\melody.phosmdl"; DestDir: "{autocf}\VST3\Phosphene.vst3\Contents\Resources"; Components: data and vst3; Check: DataReused; Flags: external ignoreversion
Source: "{app}\bass.phosmdl";   DestDir: "{autocf}\VST3\Phosphene.vst3\Contents\Resources"; Components: data and vst3; Check: DataReused; Flags: external ignoreversion
Source: "{app}\voices.phosvx";  DestDir: "{autocf}\VST3\Phosphene.vst3\Contents\Resources"; Components: data and vst3; Check: DataReused; Flags: external ignoreversion
; The offline renderer. It finds the three files above because it declares its own directory as the
; search path (Tools/render/main.cpp, installDataSearchPath) -- so it works from any shell, not only
; from the install folder.
Source: "{#Stage}\phos_render.exe"; DestDir: "{app}"; Components: render; Flags: ignoreversion
; The VST3 is a bundle: a folder the host reads as one plug-in. It is staged complete, data files
; and all, so this is a single recursive line.
Source: "{#Stage}\Phosphene.vst3\*"; DestDir: "{autocf}\VST3\Phosphene.vst3"; \
    Excludes: "*.phoswt,*.phosmdl,*.phosvx"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
; The PDF only. The HTML the same generator writes points at the screenshot PNGs by relative path,
; so it would drag two more megabytes of pictures along to say what the PDF already carries.
Source: "{#Stage}\Phosphene-Manual.pdf";  DestDir: "{app}"; Components: manual; Flags: ignoreversion skipifsourcedoesntexist
; The headset build. Nothing here can install it -- that needs a cable, developer mode and adb --
; so it is put on the disk with the line that installs it, and the shortcut below opens the folder.
Source: "{#Stage}\PhospheneQuest.apk"; DestDir: "{app}"; Components: quest; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\Phosphene.exe"; Components: standalone
Name: "{group}\Manual"; Filename: "{app}\Phosphene-Manual.pdf"; Components: manual; \
    Check: FileExists(ExpandConstant('{app}\Phosphene-Manual.pdf'))
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Phosphene.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Phosphene.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent; Components: standalone

[UninstallDelete]
; Only what this installer made. A .phosset the user saved into {app}, or a render they put there,
; is theirs and stays -- which is why {app} itself is left to Inno's own "remove it if it is empty".
Type: filesandordirs; Name: "{autocf}\VST3\Phosphene.vst3"
Type: files;          Name: "{app}\library.phoswt"
Type: files;          Name: "{app}\*.phosmdl"
Type: files;          Name: "{app}\CREDITS-wavetables.md"
Type: files;          Name: "{app}\voices.phosvx"
Type: files;          Name: "{app}\CREDITS-voices.md"
Type: files;          Name: "{app}\Phosphene-Manual.pdf"
Type: files;          Name: "{app}\PhospheneQuest.apk"
Type: files;          Name: "{app}\phosphene.ico"
Type: filesandordirs; Name: "{autoappdata}\Phosphene\field"

[Code]
var
  DownloadPage: TDownloadWizardPage;
  DataState: Integer;   // 0 not decided / no data, 1 fetched (downloaded or found beside the setup), 2 reused
  FieldState: Integer;  // the same for the field recordings (27.09.2026)

function DataFetched: Boolean; begin Result := DataState = 1; end;
function DataReused: Boolean;  begin Result := DataState = 2; end;
function DataReplaced: Boolean; begin Result := DataState = 1; end;
function FieldFetched: Boolean; begin Result := FieldState = 1; end;

// The field recordings of this set are installed: their credits file carries the set's hash.
function FieldAlreadyInstalled: Boolean;
var
  F: String;
begin
  Result := False;
  F := ExpandConstant('{autoappdata}\Phosphene\field\CREDITS-field.md');
  if not FileExists(F) then Exit;
  try
    Result := CompareText(GetSHA256OfFile(F), '{#FieldMarkerSha256}') = 0;
  except
    Result := False;
  end;
end;

// One archive of the set: from beside the setup when it is there with the right hash, else to be downloaded.
function FieldLocal(const Name, Hash: String): Boolean;
var
  Local: String;
begin
  Result := False;
  Local := ExpandConstant('{src}\') + Name;
  if FileExists(Local) and (CompareText(GetSHA256OfFile(Local), Hash) = 0) then
    Result := FileCopy(Local, ExpandConstant('{tmp}\') + Name, False);
end;

function FetchField: Boolean;
var
  NeedA, NeedB: Boolean;
begin
  Result := True;
  FieldState := 0;
  if FieldAlreadyInstalled then Exit;
  NeedA := not FieldLocal('{#FieldZipA}', '{#FieldZipASha256}');
  NeedB := not FieldLocal('{#FieldZipB}', '{#FieldZipBSha256}');
  if not (NeedA or NeedB) then begin FieldState := 1; Exit; end;
  DownloadPage.Clear;
  if NeedA then DownloadPage.Add('{#FieldBaseUrl}/{#FieldZipA}', '{#FieldZipA}', '{#FieldZipASha256}');
  if NeedB then DownloadPage.Add('{#FieldBaseUrl}/{#FieldZipB}', '{#FieldZipB}', '{#FieldZipBSha256}');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      FieldState := 1;
    except
      if DownloadPage.AbortedByUser then
        Result := False
      else
        Result := SuppressibleMsgBox(FmtMessage(CustomMessage('FieldDownloadFailed'), [GetExceptionMessage]),
                                     mbError, MB_YESNO or MB_DEFBUTTON1, IDYES) = IDYES;
    end;
  finally
    DownloadPage.Hide;
  end;
end;

// Every data file in {app} with the hash this release expects.
function DataAlreadyInstalled: Boolean;
var
  Names, Hashes: array of String;
  I: Integer;
  F: String;
begin
  SetArrayLength(Names, {#DataFileCount});
  SetArrayLength(Hashes, {#DataFileCount});
  Names[0] := '{#DataFile0}'; Hashes[0] := '{#DataHash0}';
  Names[1] := '{#DataFile1}'; Hashes[1] := '{#DataHash1}';
  Names[2] := '{#DataFile2}'; Hashes[2] := '{#DataHash2}';
  Names[3] := '{#DataFile3}'; Hashes[3] := '{#DataHash3}';
  Names[4] := '{#DataFile4}'; Hashes[4] := '{#DataHash4}';
  Names[5] := '{#DataFile5}'; Hashes[5] := '{#DataHash5}';
  Result := True;
  for I := 0 to {#DataFileCount} - 1 do begin
    F := ExpandConstant('{app}\') + Names[I];
    if not FileExists(F) then begin Result := False; Exit; end;
    try
      if CompareText(GetSHA256OfFile(F), Hashes[I]) <> 0 then begin Result := False; Exit; end;
    except
      Result := False; Exit;
    end;
  end;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
  DataState := 0;
  FieldState := 0;
end;

// At "Ready": reuse what is installed, else take the archive from beside the setup, else download it.
function NextButtonClick(CurPageID: Integer): Boolean;
var
  Local: String;
begin
  Result := True;
  if CurPageID <> wpReady then Exit;
  // The field recordings first: their own archives, their own question when the download fails.
  if WizardIsComponentSelected('field') then begin
    Result := FetchField;
    if not Result then Exit;
  end;
  if not WizardIsComponentSelected('data') then Exit;
  DataState := 0;
  if DataAlreadyInstalled then begin
    DataState := 2;
    Exit;
  end;
  Local := ExpandConstant('{src}\{#DataZip}');
  if FileExists(Local) and (CompareText(GetSHA256OfFile(Local), '{#DataZipSha256}') = 0) then begin
    if FileCopy(Local, ExpandConstant('{tmp}\{#DataZip}'), False) then begin
      DataState := 1;
      Exit;
    end;
  end;
  DownloadPage.Clear;
  DownloadPage.Add('{#DataBaseUrl}/{#DataZip}', '{#DataZip}', '{#DataZipSha256}');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
      DataState := 1;
    except
      if DownloadPage.AbortedByUser then
        Result := False
      else
        Result := SuppressibleMsgBox(FmtMessage(CustomMessage('DownloadFailed'), [GetExceptionMessage]),
                                     mbError, MB_YESNO or MB_DEFBUTTON1, IDYES) = IDYES;
    end;
  finally
    DownloadPage.Hide;
  end;
end;

// AVX2 is a hard requirement of the shipped binaries (PHOS_AVX2=ON), and a processor without it
// does not fail gracefully: it takes an illegal instruction and dies with no explanation. Asked
// here, where there is still somewhere to say it. Answered rather than enforced -- the query is a
// Windows feature flag, and an old Windows saying "no" is not the same as the processor lacking it.
function IsProcessorFeaturePresent(Feature: DWord): Boolean;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsProcessorFeaturePresent(40) then                    // PF_AVX2_INSTRUCTIONS_AVAILABLE
    Result := MsgBox(CustomMessage('NoAvx2'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;

function FullTypeName(Param: String): String;
begin
  if ActiveLanguage = 'de' then Result := 'Vollstaendig' else Result := 'Full installation';
end;

function CustomTypeName(Param: String): String;
begin
  if ActiveLanguage = 'de' then Result := 'Benutzerdefiniert' else Result := 'Custom installation';
end;
