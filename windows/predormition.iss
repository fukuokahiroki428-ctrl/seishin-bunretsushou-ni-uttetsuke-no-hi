; Predormition Windows 단일 설치 파일 (Inno Setup 6)
; GitHub Actions 의 Deploy 단계가 만든 dist\win\ 전체를 하나의 Predormition_Setup.exe 로 패키징.
; iscc 는 repo 루트에서 호출 → 모든 상대경로는 repo 루트 기준.

#define MyAppName    "Predormition"
; ★ 버전은 CI 가 레포 루트 VERSION 파일을 읽어 /DMyAppVersion=... 로 넘긴다.
;   아래 값은 로컬에서 iscc 를 직접 돌릴 때만 쓰이는 폴백이다.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-local"
#endif
#define MyAppExeName "Predormition.exe"

[Setup]
AppId={{7F2C9A64-51D8-4B3E-9E17-2A6D8C40B915}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Predormition
; 관리자 권한 없이 사용자 폴더에 설치 (UAC 안 뜸, 앱이 자기 폴더에 쓰기 가능)
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\Predormition
DisableProgramGroupPage=yes
DisableDirPage=auto
OutputDir=installer_out
OutputBaseFilename=Predormition_Setup
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
; dist\win 전체 (스테이징 폴더는 MAX_PATH 여유 위해 짧게) (exe + Qt DLL + tools + python_env)
Source: "dist\win\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Predormition"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\Predormition"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,Predormition}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; ★ 앱이 설치 폴더 안에 런타임으로 만드는 것들(파이썬 캐시·로그·도구 갱신본)은
;   Inno 가 추적하지 못해 제거 후에도 남는다 → 명시적으로 지운다.
Type: filesandordirs; Name: "{app}\python_env"
Type: filesandordirs; Name: "{app}\tools"
Type: files;          Name: "{app}\*.log"
; ★ 진단 로그의 실제 이름은 chernobyl_log.txt / predormition_log.txt — 확장자가 .txt 라
;   위의 *.log 글롭에 걸리지 않는다. 실제로 제거 후 chernobyl_log.txt 가 남았고, 그 파일
;   하나 때문에 아래 dirifempty 도 설치 폴더를 지우지 못했다.
;   로그 위치는 AppData 로 옮겼지만 AppDataLocation 을 못 얻으면 설치 폴더로 폴백하므로
;   (main.cpp 의 chernobylLogHandler) 여기서 계속 정리해야 한다. 회전본 .1 까지 포함.
Type: files;          Name: "{app}\*_log.txt"
Type: files;          Name: "{app}\*_log.txt.1"
Type: dirifempty;     Name: "{app}"
