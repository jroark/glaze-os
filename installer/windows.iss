[Setup]
AppName=Glaze BE-300 Installer
AppVersion=0.1.0
DefaultDirName={localappdata}\Programs\Glaze Installer
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
OutputBaseFilename=Glaze-Installer-windows-x64
UninstallDisplayIcon={app}\Glaze Installer.exe
[Files]
Source: "..\out\installer\dist\Glaze Installer\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs
[Icons]
Name: "{autoprograms}\Glaze BE-300 Installer"; Filename: "{app}\Glaze Installer.exe"
