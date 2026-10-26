; Inno Setup script for the Windows installer. Built by .github/workflows/release.yml:
;   iscc /DAppVersion=1.2.3 /DStage=<folder from make_package.py> /DRedist=<vc_redist.x64.exe> /DOut=<dir> installer.iss
; It installs the launcher for the current user (no administrator rights; forza.toml sits in the
; install folder), and the Microsoft Visual C++ 2015-2022 x64 redistributable when it is missing.
; The installer contains no game code or data.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppName "Forza Horizon 1: ReImagined"
; Folder and shortcut names: Windows does not allow ":" in them.
#define ShortName "Forza Horizon 1 - ReImagined"

[Setup]
AppId={{B0C6F2A4-5D1E-4D0B-9B1B-7B5A0F3E6C21}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Dyotson
AppPublisherURL=https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined
DefaultDirName={localappdata}\Programs\ForzaHorizon1-ReImagined
DefaultGroupName={#ShortName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Out}
OutputBaseFilename=ForzaHorizon1-ReImagined-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=../../forza-recomp/resources/forza.ico
UninstallDisplayIcon={app}\forza-launcher.exe
LicenseFile={#Stage}\licenses\ForzaHorizon1-ReImagined.txt
DisableProgramGroupPage=yes
; Always show where the files go and let the player change it, also on updates (Inno Setup's
; default skips the page once a previous install exists, issue #403). The launcher's own update
; passes /DIR, so the page comes up filled with the current install folder.
DisableDirPage=no
; Installed files keep the build's real (UTC) file times. By default Inno Setup stores the build machine's
; local time (UTC on CI) and sets it as the player's local time: hours in the future west of UTC, which
; keeps ninja re-running CMake (issue #35).
TimeStampsInUTC=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
#ifdef Redist
Source: "{#Redist}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: NeedsVCRedist
#endif

[Icons]
Name: "{autoprograms}\{#ShortName}"; Filename: "{app}\forza-launcher.exe"
Name: "{autodesktop}\{#ShortName}"; Filename: "{app}\forza-launcher.exe"; Tasks: desktopicon

[Run]
#ifdef Redist
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Flags: shellexec waituntilterminated; Check: NeedsVCRedist
#endif
Filename: "{app}\forza-launcher.exe"; Description: "Start the launcher"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsVCRedist: Boolean;
var
  Installed: Cardinal;
begin
  Result := True;
  if RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) then
    Result := Installed <> 1;
end;
