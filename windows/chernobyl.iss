; Chernobyl Windows 단일 설치 파일 (Inno Setup 6)
; GitHub Actions 의 Deploy 단계가 만든 dist\Chernobyl_win\ 전체를 하나의 Chernobyl_Setup.exe 로 패키징.
; iscc 는 repo 루트에서 호출 → 모든 상대경로는 repo 루트 기준.

#define MyAppName    "Chernobyl"
#define MyAppVersion "1.0"
#define MyAppExeName "Chernobyl.exe"

[Setup]
AppId={{B8E4F2A1-9C3D-4E5F-A6B7-C8D9E0F1A2B3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Chernobyl
; 관리자 권한 없이 사용자 폴더에 설치 (UAC 안 뜸, 앱이 자기 폴더에 쓰기 가능)
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\Chernobyl
DisableProgramGroupPage=yes
DisableDirPage=auto
OutputDir=installer_out
OutputBaseFilename=Chernobyl_Setup
SetupIconFile=resources\abiwa.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; dist\Chernobyl_win 전체 (exe + Qt DLL + tools + python_env)
Source: "dist\Chernobyl_win\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Chernobyl"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\Chernobyl"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,Chernobyl}"; Flags: nowait postinstall skipifsilent
