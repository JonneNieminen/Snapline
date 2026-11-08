; Snapline Windows installer (VST3 only), built with Inno Setup 6.
; iscc /DAppVersion=1.2.3 /DSourceDir=<folder containing Snapline.vst3> /DOutputDir=<dist> build\windows\snapline.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\Snapline_artefacts\Release\VST3"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{DAF19CD1-B666-4F55-A29B-66D6292A5B98}
AppName=Snapline
AppVersion={#AppVersion}
AppVerName=Snapline {#AppVersion}
AppPublisher=Snapline Audio
AppPublisherURL=https://github.com/JonneNieminen/Snapline
AppSupportURL=https://github.com/JonneNieminen/Snapline/issues
AppUpdatesURL=https://github.com/JonneNieminen/Snapline/releases
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
DirExistsWarning=no
UninstallFilesDir={commonappdata}\Snapline
UninstallDisplayName=Snapline (VST3)
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=Snapline-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Messages]
FinishedLabel=Snapline is installed in Common Files\VST3. Rescan plugins in your DAW to use it.

[Files]
Source: "{#SourceDir}\Snapline.vst3\*"; DestDir: "{commoncf64}\VST3\Snapline.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Snapline.vst3"
