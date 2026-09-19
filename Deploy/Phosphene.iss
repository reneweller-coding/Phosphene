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
; so all three files are installed twice, once per place. They are 3.7 MB together; a second copy is
; cheaper than a plug-in that silently plays the six built-in wavetables and the Markov model
; because a host copied the bundle somewhere without the folder next to it.

#ifndef Version
  #define Version "1.0.0"
#endif
#define AppName "Phosphene"
#define Publisher "Rene Weller"
#define AppURL "https://github.com/reneweller-coding/Phosphene"
#define Stage "stage"

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
en.NoAvx2=This processor reports no AVX2 support.%n%nPhosphene is built for AVX2, which every x86-64 processor since 2013 has. Without it, it will not start.%n%nInstall anyway?
de.CompStandalone=Eigenstaendiges Programm
de.CompVst3=VST3-Plugin (fuer eine DAW)
de.CompRender=Offline-Renderer (phos_render, Kommandozeile)
de.CompManual=Handbuch (PDF)
de.CompQuest=Meta-Quest-App (APK, wird mit adb installiert)
de.TaskDesktop=Verknuepfung auf dem Desktop anlegen
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
Type: files;          Name: "{app}\library.phoswt"
Type: files;          Name: "{app}\*.phosmdl"
Type: files;          Name: "{app}\Phosphene-Manual.pdf"
Type: files;          Name: "{app}\PhospheneQuest.apk"
Type: files;          Name: "{app}\CREDITS-wavetables.md"
Type: files;          Name: "{app}\voices.phosvx"
Type: files;          Name: "{app}\CREDITS-voices.md"

[Files]
Source: "{#Stage}\Phosphene.exe";  DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt";    DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\README.txt";     DestDir: "{app}"; Components: standalone; Flags: ignoreversion isreadme
Source: "{#Stage}\phosphene.ico";  DestDir: "{app}"; Components: standalone; Flags: ignoreversion
; The data the engine opens by name, beside the binaries that declare {app} as their search path.
; "standalone" is a fixed component, so these are always installed -- phos_render shares them.
Source: "{#Stage}\library.phoswt";          DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\melody.phosmdl";          DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\bass.phosmdl";            DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\CREDITS-wavetables.md";   DestDir: "{app}"; Components: standalone; Flags: ignoreversion
; 19.09.2026: the voice pack (spoken phrases, Vocal.h) and its credits.
Source: "{#Stage}\voices.phosvx";           DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Stage}\CREDITS-voices.md";       DestDir: "{app}"; Components: standalone; Flags: ignoreversion
; The offline renderer. It finds the three files above because it declares its own directory as the
; search path (Tools/render/main.cpp, installDataSearchPath) -- so it works from any shell, not only
; from the install folder.
Source: "{#Stage}\phos_render.exe"; DestDir: "{app}"; Components: render; Flags: ignoreversion
; The VST3 is a bundle: a folder the host reads as one plug-in. It is staged complete, data files
; and all, so this is a single recursive line.
Source: "{#Stage}\Phosphene.vst3\*"; DestDir: "{autocf}\VST3\Phosphene.vst3"; \
    Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
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

[Code]
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
