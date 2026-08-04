@echo off
chcp 65001 >nul
title Predormition - 로컬 AI 설치
REM 더블클릭용 런처. PowerShell 실행 정책에 막히지 않도록 우회해서 호출한다.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0win_install_ai.ps1"
