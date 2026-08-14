# ═══════════════════════════════════════════════════════════════════════════
#  로컬 AI(오픈클로) 설치 — Windows
#
#  설치본에는 앱만 들어간다. AI 모델은 1~9GB 라 설치 파일에 넣을 수 없어,
#  이 스크립트가 받아서 앱 폴더 안(<앱>\llm)에 넣는다.
#
#  - 엔진(llama-server.exe)과 모델 모두 우리 보관 릴리즈를 1순위로 받는다.
#    원 배포처가 사라져도 설치된다.
#  - 중간에 끊겨도 다시 실행하면 이어받는다(.part 로 받고 완료 시에만 이름 변경).
#  - macOS 와 달리 코드서명 재봉인이 필요 없다 — 복사로 끝.
#
#  더블클릭은 AI_설치_더블클릭.bat 을 쓴다(실행 정책 우회 포함).
# ═══════════════════════════════════════════════════════════════════════════
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$AppName       = 'Predormition'
$EngineMirror  = 'https://github.com/fukuokahiroki428-ctrl/seishin-bunretsushou-ni-uttetsuke-no-hi/releases/download/ai-engines-v1'
$ModelMirror   = 'https://github.com/fukuokahiroki428-ctrl/seishin-bunretsushou-ni-uttetsuke-no-hi/releases/download/ai-assets-v1'
$HF            = 'https://huggingface.co/Qwen'

function Exit-Script([int]$code = 0) {
    Write-Host ''
    Read-Host '엔터를 눌러 닫기' | Out-Null
    exit $code
}

# ── SHA256 목록 받기 ───────────────────────────────────────────────────────
#   ★ GitHub 릴리즈 자산은 Content-Type 이 application/octet-stream 이라
#     PowerShell 5.1 의 Invoke-WebRequest 가 .Content 를 Byte[] 로 준다.
#     이걸 문자열로 알고 -split/-match 하면 전부 빗나가고, 예외도 안 나서
#     검증 블록이 조용히 통째로 건너뛰어진다(실측 확인). 반드시 디코드해서 쓴다.
function Get-Sha256Map([string]$url) {
    try {
        $raw = (Invoke-WebRequest $url -UseBasicParsing -TimeoutSec 30).Content
        if ($raw -is [byte[]]) { $raw = [Text.Encoding]::UTF8.GetString($raw) }
        $map = @{}
        foreach ($ln in ($raw -split "`r?`n")) {
            # "<해시>  <파일명>" 또는 "<해시> *<파일명>"
            if ($ln -match '^\s*([0-9a-fA-F]{64})\s+\*?(.+?)\s*$') { $map[$Matches[2]] = $Matches[1].ToLower() }
        }
        if ($map.Count -eq 0) { return $null }
        return $map
    } catch { return $null }
}

# 받은 파일을 목록과 대조한다. 목록에 없으면(원 배포처에서 받은 경우 등) 통과시킨다.
#   불일치면 파일을 지운다 — 남겨두면 다음 실행에서 "이미 있음" 으로 건너뛰어
#   손상된 파일이 영구히 자리를 차지한다.
function Confirm-Sha256([string]$path, [string]$name, $map, [string]$label) {
    if (-not $map) { Write-Host "  ⚠ $label 체크섬 목록을 얻지 못했습니다 — 검증 생략"; return $true }
    if (-not $map.ContainsKey($name)) { Write-Host "  ⚠ $label 체크섬 목록에 $name 이(가) 없습니다 — 검증 생략"; return $true }
    Write-Host '  · 무결성 확인 중...'
    $got = (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
    if ($got -eq $map[$name]) { Write-Host '  ✔ 체크섬 확인'; return $true }
    Write-Host "  ✗ 체크섬 불일치 — 받은 파일이 손상됐습니다. 지우고 중단합니다."
    Write-Host "     기대: $($map[$name])"
    Write-Host "     실제: $got"
    Remove-Item $path -Force -ErrorAction SilentlyContinue
    return $false
}

Write-Host ''
Write-Host '════════════════════════════════════════════'
Write-Host "  $AppName — 로컬 AI(오픈클로) 설치"
Write-Host '════════════════════════════════════════════'
Write-Host ''

# ── 0) curl.exe 확인 ───────────────────────────────────────────────────────
#   Windows 10 1803+ / 11 에 기본 포함. 이어받기(-C -)가 필요해 이걸 쓴다.
$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue)
if (-not $curl) {
    Write-Host '이 스크립트는 curl.exe 가 필요합니다 (Windows 10 1803 이상에 기본 포함).'
    Write-Host 'Windows 를 업데이트하거나 curl 을 설치한 뒤 다시 실행해 주세요.'
    Exit-Script 1
}

# ── 1) 앱 찾기 ─────────────────────────────────────────────────────────────
#   ★ 설치 폴더는 사용자가 바꿀 수 있다(predormition.iss 의 DisableDirPage=auto).
#     기본 위치만 뒤지면 "앱이 깔려 있는데 먼저 설치하라" 는 말을 듣게 된다(실측).
#     Inno Setup 이 남긴 InstallLocation 을 1순위로 본다.
$fromRegistry = @()
foreach ($root in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
                    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
                    'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        if ($p -and $p.DisplayName -like "$AppName*" -and $p.InstallLocation) {
            $fromRegistry += $p.InstallLocation.TrimEnd('\')
        }
    }
}

$candidates = $fromRegistry + @(
    (Join-Path $env:LOCALAPPDATA "Programs\$AppName"),
    (Join-Path ${env:ProgramFiles} $AppName),
    (Join-Path ${env:ProgramFiles(x86)} $AppName),
    $PSScriptRoot,
    (Split-Path -Parent $PSScriptRoot)   # scripts\ 안에서 실행된 경우
)
$appDir = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c "$AppName.exe"))) { $appDir = $c; break }
}
if (-not $appDir) {
    Write-Host "먼저 $AppName 을 설치한 뒤 이 파일을 다시 실행해 주세요."
    Write-Host '설치돼 있는데도 이 메시지가 나오면, 이 파일을 앱 폴더'
    Write-Host "($AppName.exe 가 있는 곳) 안에 두고 다시 실행해 주세요."
    Write-Host '(설치본: Predormition_Setup.exe)'
    Exit-Script 1
}
Write-Host "앱 위치: $appDir"

$llmDir = Join-Path $appDir 'llm'
try { New-Item -ItemType Directory -Force -Path $llmDir -ErrorAction Stop | Out-Null }
catch {
    Write-Host ''
    Write-Host '앱 폴더에 쓸 권한이 없습니다.'
    Write-Host '이 파일을 마우스 오른쪽 → "관리자 권한으로 실행" 으로 다시 실행해 주세요.'
    Exit-Script 1
}

# ── 2) 무엇을 받을지 ───────────────────────────────────────────────────────
Write-Host ''
Write-Host '설치할 AI 를 고르세요. (숫자 입력 후 Enter)'
Write-Host ''
Write-Host '  1) 기본        1.0GB  — 대화용. 가볍고 빠름.'
Write-Host '  2) 코드 수리   3.0GB  — 기본 + 코드 자가수리용(권장).'
Write-Host '  3) 전체        9.3GB  — 위 전부 + 최고 품질 7B.'
Write-Host ''
$choice = Read-Host '선택 [2]'
if ([string]::IsNullOrWhiteSpace($choice)) { $choice = '2' }

$M15  = 'qwen2.5-1.5b-instruct-q4_k_m.gguf'
$M3   = 'qwen2.5-3b-instruct-q4_k_m.gguf'
$MC3  = 'qwen2.5-coder-3b-instruct-q4_k_m.gguf'
$MC7A = 'qwen2.5-coder-7b-instruct-q4_k_m-00001-of-00002.gguf'
$MC7B = 'qwen2.5-coder-7b-instruct-q4_k_m-00002-of-00002.gguf'

# 원 배포처(허깅페이스) 경로 — 보관본이 없을 때만 쓴다.
#   (해시테이블 키에 변수를 쓰면 버전에 따라 해석이 달라질 수 있어 문자열로 명시한다)
$hfRepo = @{
    'qwen2.5-1.5b-instruct-q4_k_m.gguf'                        = 'Qwen2.5-1.5B-Instruct-GGUF'
    'qwen2.5-3b-instruct-q4_k_m.gguf'                          = 'Qwen2.5-3B-Instruct-GGUF'
    'qwen2.5-coder-3b-instruct-q4_k_m.gguf'                    = 'Qwen2.5-Coder-3B-Instruct-GGUF'
    'qwen2.5-coder-7b-instruct-q4_k_m-00001-of-00002.gguf'     = 'Qwen2.5-Coder-7B-Instruct-GGUF'
    'qwen2.5-coder-7b-instruct-q4_k_m-00002-of-00002.gguf'     = 'Qwen2.5-Coder-7B-Instruct-GGUF'
}

switch ($choice) {
    '1'     { $models = @($M15) }
    '3'     { $models = @($M15, $M3, $MC3, $MC7A, $MC7B) }
    default { $models = @($M15, $MC3) }
}

# ── 3) 엔진(llama-server.exe) ──────────────────────────────────────────────
$srv = Join-Path $llmDir 'llama-server.exe'
if (-not (Test-Path $srv)) {
    Write-Host ''
    Write-Host '▶ 추론 엔진(llama-server) 설치 중...'
    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $engineName = "llama-engine-win-$arch.zip"
    $tmp = Join-Path $env:TEMP ("pred_ai_" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $zip = Join-Path $tmp $engineName

    # 보관본 → 없으면 원 배포처 최신
    $url = "$EngineMirror/$engineName"
    & curl.exe -fsIL $url *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host '   보관본이 없어 원 배포처에서 찾습니다...'
        try {
            $rel = Invoke-RestMethod 'https://api.github.com/repos/ggml-org/llama.cpp/releases/latest'
            $pat = "win-cpu-$arch"
            $url = ($rel.assets | Where-Object { $_.name -like "*$pat*" } | Select-Object -First 1).browser_download_url
        } catch { $url = $null }
    }
    if (-not $url) {
        Write-Host '  ✗ 엔진 다운로드 주소를 찾지 못했습니다. 인터넷 연결을 확인해 주세요.'
        Exit-Script 1
    }

    & curl.exe -fL --retry 3 -o $zip $url
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $zip)) {
        Write-Host '  ✗ 엔진 내려받기 실패'
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        Exit-Script 1
    }

    # 무결성 대조 — 보관본일 때만(체크섬 파일이 같은 릴리즈에 있다).
    if (-not (Confirm-Sha256 $zip $engineName (Get-Sha256Map "$EngineMirror/ENGINES_SHA256.txt") '엔진')) {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        Exit-Script 1
    }

    Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp 'x') -Force
    # 압축 구조가 배포처마다 달라 이름으로 찾는다.
    $found = Get-ChildItem (Join-Path $tmp 'x') -Recurse -Filter 'llama-server.exe' | Select-Object -First 1
    if ($found) {
        Copy-Item $found.FullName $srv -Force
        # 같은 폴더의 DLL 도 함께(런타임 의존)
        Get-ChildItem $found.Directory -Filter '*.dll' | ForEach-Object {
            Copy-Item $_.FullName (Join-Path $llmDir $_.Name) -Force
        }
    }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

    if (Test-Path $srv) { Write-Host '  ✔ 엔진 설치 완료' }
    else { Write-Host '  ✗ 압축 안에서 llama-server.exe 를 찾지 못했습니다'; Exit-Script 1 }
} else {
    Write-Host '▶ 추론 엔진 이미 설치됨 — 건너뜀'
}

# ── 4) 모델 ────────────────────────────────────────────────────────────────
# ★ 받는 곳마다 임시 파일을 따로 쓴다. 하나가 실패하며 지울 때 다른 경로가
#   받다 만 것까지 지워지면 이어받기가 매번 처음부터 되기 때문이다.
$fail = $false
# ★ 모델도 무결성을 확인한다. 최대 3.8GB 를 받으면서 지금까지 아무 대조도 하지
#   않았다 — 조각을 이어붙이는 경로가 있어 중간에 하나만 어긋나도 조용히 깨진
#   모델이 만들어진다. 목록은 한 번만 받아 재사용한다.
$modelSums = Get-Sha256Map "$ModelMirror/MODELS_SHA256.txt"
foreach ($name in $models) {
    $out = Join-Path $llmDir $name
    if (Test-Path $out) { Write-Host "▶ $name — 이미 있음, 건너뜀"; continue }

    Write-Host ''
    Write-Host "▶ 다운로드: $name"
    $mirTmp = "$out.mirror.part"
    $mrgTmp = "$out.merge.part"
    $hfTmp  = "$out.part"

    # ① 보관본 통째로
    & curl.exe -fL --retry 3 -C - --progress-bar -o $mirTmp "$ModelMirror/$name"
    if ($LASTEXITCODE -eq 0 -and (Test-Path $mirTmp)) {
        Move-Item $mirTmp $out -Force
        if (-not (Confirm-Sha256 $out $name $modelSums '모델')) { $fail = $true; continue }
        Write-Host '  ✔ 완료(보관본)'
        continue
    }

    # ② 보관본 분할 (.partaa/.partab/...)
    $parts = @()
    $ok = $true
    foreach ($sfx in @('aa','ab','ac','ad','ae','af')) {
        & curl.exe -fsIL "$ModelMirror/$name.part$sfx" *> $null
        if ($LASTEXITCODE -ne 0) { break }
        Write-Host "   조각 part$sfx 받는 중..."
        $p = "$out.chunk$sfx"
        & curl.exe -fL --retry 3 -C - --progress-bar -o $p "$ModelMirror/$name.part$sfx"
        if ($LASTEXITCODE -ne 0) { $ok = $false; break }
        $parts += $p
    }
    if ($parts.Count -gt 0 -and $ok) {
        try {
            # 큰 파일이라 통째로 읽지 않고 스트림으로 이어붙인다(메모리 폭발 방지).
            $fs = [System.IO.File]::Create($mrgTmp)
            foreach ($p in $parts) {
                $in = [System.IO.File]::OpenRead($p)
                $in.CopyTo($fs, 4MB)
                $in.Close()
            }
            $fs.Close()
            $parts | ForEach-Object { Remove-Item $_ -Force -ErrorAction SilentlyContinue }
            Move-Item $mrgTmp $out -Force
            # 조각을 이어붙인 경로라 여기가 특히 중요하다 — 하나만 어긋나도 파일은 만들어진다.
            if (-not (Confirm-Sha256 $out $name $modelSums '모델')) { $fail = $true; continue }
            Write-Host '  ✔ 완료(보관본 조각 합침)'
            continue
        } catch {
            Write-Host "  ⚠ 조각 합치기 실패: $($_.Exception.Message)"
            if ($fs) { $fs.Close() }
        }
    }
    $parts | ForEach-Object { Remove-Item $_ -Force -ErrorAction SilentlyContinue }
    Remove-Item $mirTmp, $mrgTmp -Force -ErrorAction SilentlyContinue

    # ③ 원 배포처(허깅페이스)
    Write-Host '   보관본이 없어 원 배포처에서 받습니다...'
    $repo = $hfRepo[$name]
    & curl.exe -fL --retry 3 -C - --progress-bar -o $hfTmp "$HF/$repo/resolve/main/$name"
    if ($LASTEXITCODE -eq 0 -and (Test-Path $hfTmp)) {
        Move-Item $hfTmp $out -Force
        # 원 배포처에서 받은 것도 보관본과 같은 파일이면 목록에 있다. 없으면 검증을 건너뛴다.
        if (-not (Confirm-Sha256 $out $name $modelSums '모델')) { $fail = $true; continue }
        Write-Host '  ✔ 완료'
    } else {
        # 부분 파일은 남겨둔다 — 다시 실행하면 -C - 가 이어받는다.
        Write-Host '  ✗ 실패 — 다시 실행하면 받다 만 지점부터 이어받습니다'
        $fail = $true
    }
}

# ── 5) 마무리 ──────────────────────────────────────────────────────────────
Write-Host ''
Write-Host '════════════════════════════════════════════'
$ggufCount = (Get-ChildItem $llmDir -Filter '*.gguf' -ErrorAction SilentlyContinue).Count
if (-not $fail -and $ggufCount -gt 0 -and (Test-Path $srv)) {
    Write-Host '  ✅ 설치 완료!'
    Write-Host "  엔진 1개, 모델 $ggufCount 개"
    Write-Host '  앱을 열고 설정 → 로컬 AI 에서 "AI 켜기" 를 누르세요.'
    Write-Host '  대화창에 "코드 고쳐줘" 라고 하면 스스로 점검·수리합니다.'
} else {
    Write-Host '  ⚠ 일부 파일을 받지 못했습니다.'
    Write-Host '  이 파일을 다시 실행하면 못 받은 것만 이어받습니다.'
}
Write-Host '════════════════════════════════════════════'
Exit-Script 0
