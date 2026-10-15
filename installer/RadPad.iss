; Inno Setup script for RadPad. The Release build runs it (see the CreateInstaller target in
; RadPad\RadPad.vcxproj), passing these on the command line:
;   /DAppVersion=1.0.0  /DPlatform=x64  /DSourceDir=<the build's output folder>  /DOutputDir=<where to write setup>
; It can also be built by hand with ISCC.exe from the repository root, after a Release build.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef Platform
  #define Platform "x64"
#endif
#ifndef SourceDir
  #define SourceDir "..\x64\Release\RadPad"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
; AppId identifies RadPad to Windows across versions, so a new installer upgrades the old install.
; Never change it.
AppId={{6E3F2B8A-9C41-4D7E-A5B2-3F8C1D0E7A64}
AppName=RadPad
AppVersion={#AppVersion}
AppVerName=RadPad {#AppVersion}
VersionInfoVersion={#AppVersion}
; Installs for the current user, without needing administrator rights, into
; %LOCALAPPDATA%\Programs\RadPad. The user can choose to install for everyone instead.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\RadPad
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=RadPad-{#Platform}-setup
SetupIconFile=..\RadPad\RadPad.ico
UninstallDisplayIcon={app}\RadPad.exe
UninstallDisplayName=RadPad
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Close a running RadPad before replacing its files.
CloseApplications=force
ChangesAssociations=yes
#if Platform == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#elif Platform == "ARM64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#endif
MinVersion=10.0.17763

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "openwith"; Description: "Add RadPad to ""Open with"" for text and code files"
Name: "contextmenu"; Description: "Add ""Edit with RadPad"" when right-clicking any file"; Flags: unchecked

[Files]
; Everything the build put in the output folder, apart from files only used during development.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb,*.lib,*.exp,RadPad.winmd,*.ico"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\RadPad"; Filename: "{app}\RadPad.exe"
Name: "{autodesktop}\RadPad"; Filename: "{app}\RadPad.exe"; Tasks: desktopicon

[Registry]
; "Open with": a file type for RadPad, listed for each extension without becoming its default program.
; HKA is HKEY_CURRENT_USER for a per-user install and HKEY_LOCAL_MACHINE for an all-users one.
Root: HKA; Subkey: "Software\Classes\RadPad.Document"; ValueType: string; ValueData: "RadPad document"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\RadPad.Document\DefaultIcon"; ValueType: string; ValueData: "{app}\RadPad.exe,0"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\RadPad.Document\shell\open\command"; ValueType: string; ValueData: """{app}\RadPad.exe"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\RadPad.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "RadPad"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\RadPad.exe\shell\open\command"; ValueType: string; ValueData: """{app}\RadPad.exe"" ""%1"""; Tasks: openwith
#define Extensions "txt log md markdown text bat c c++ cc cfg cjs cmd conf config cpp cs csproj css csx cts cxx editorconfig filters gitconfig go h hh hpp htm html hxx idl inf ini inl ipp ixx java js json jsonc jsx less manifest mjs mts nuspec plist properties props ps1 psd1 psm1 py pyi pyw rc rc2 reg resx rs scss sql svg targets ts tsx vbproj vcxproj webmanifest xaml xhtml xml xsd xsl xslt"
#define Rest Extensions + " "
#sub AddExtension
  #define Ext Copy(Rest, 1, Pos(" ", Rest) - 1)
  #define Rest Copy(Rest, Pos(" ", Rest) + 1)
Root: HKA; Subkey: "Software\Classes\.{#Ext}\OpenWithProgids"; ValueType: string; ValueName: "RadPad.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\RadPad.exe\SupportedTypes"; ValueType: string; ValueName: ".{#Ext}"; ValueData: ""; Tasks: openwith
#endsub
#for {0; Len(Rest) > 0; 0} AddExtension
; "Edit with RadPad" for any file. On Windows 11 it's under "Show more options".
Root: HKA; Subkey: "Software\Classes\*\shell\RadPad"; ValueType: string; ValueData: "Edit with RadPad"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\*\shell\RadPad"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\RadPad.exe,0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\*\shell\RadPad\command"; ValueType: string; ValueData: """{app}\RadPad.exe"" ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\RadPad.exe"; Description: "Start RadPad"; Flags: nowait postinstall skipifsilent

; Settings in %LOCALAPPDATA%\RadPad are the user's, so uninstalling leaves them.
