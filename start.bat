@echo off
rem ============================================================
rem  Campus Card System - Quick Start
rem  This file is pure ASCII on purpose: cmd reads batch files
rem  using the console code page, so non-ASCII text here breaks
rem  parsing under different locales. The program itself prints
rem  Chinese correctly (it sets the console to UTF-8).
rem  Usage: double-click, or pass a port:  start.bat 9000
rem ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo   Campus Card / Yi Ka Tong  -  starting
echo ============================================================
echo.

rem ---------- 1. locate a compiler ----------
set "GCC="
where gcc >nul 2>nul && set "GCC=gcc"
if not defined GCC if exist "C:\Program Files\mingw64\bin\gcc.exe" set "GCC=C:\Program Files\mingw64\bin\gcc.exe"
if not defined GCC if exist "C:\mingw64\bin\gcc.exe" set "GCC=C:\mingw64\bin\gcc.exe"
if not defined GCC if exist "E:\mingw64\bin\gcc.exe" set "GCC=E:\mingw64\bin\gcc.exe"

rem ---------- 2. build when server.exe is missing ----------
if exist "server.exe" goto READY
if not defined GCC goto NOGCC

echo [1/2] compiling ... (source is UTF-8, needs -fexec-charset=UTF-8)
"%GCC%" -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 -Isrc src\server.c src\campus.c -o server.exe -lws2_32
if errorlevel 1 goto BUILDFAIL
echo       build OK.
echo.
goto READY

:NOGCC
echo [ERROR] gcc not found and server.exe is missing.
echo.
echo   Option 1: install MinGW-w64, then run this script again.
echo   Option 2: put a prebuilt server.exe into this folder.
goto END

:BUILDFAIL
echo.
echo [ERROR] build failed, see the messages above.
goto END

:READY
rem ---------- 3. local LAN IP ----------
set "LANIP="
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do (
    if not defined LANIP set "LANIP=%%a"
)
set "LANIP=%LANIP: =%"
if not defined LANIP set "LANIP=127.0.0.1"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=8090"

echo [2/2] serving on 0.0.0.0:%PORT%  (reachable from your LAN)
echo.
echo   ------------------------------------------------------------
echo     This computer   http://127.0.0.1:%PORT%/
echo     Phone / LAN     http://%LANIP%:%PORT%/     (same WiFi)
echo   ------------------------------------------------------------
echo     Demo accounts
echo       Student      C001 / C002 / C003     login password 123456
echo       Pay password 010101 / 010102 / 010103
echo       Admin        admin123
echo   ------------------------------------------------------------
echo     Close this window to stop. Data is kept in data\
echo   ------------------------------------------------------------
echo.

start "" "http://127.0.0.1:%PORT%/"
server.exe %PORT% web data 0.0.0.0

:END
echo.
pause