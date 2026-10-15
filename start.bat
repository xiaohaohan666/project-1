@echo off
rem ============================================================
rem  校园一卡通 —— 一键启动
rem
rem  本文件以 GBK 保存：cmd 按系统 ANSI 代码页读取批处理文件，
rem  保存成 GBK 它才能正确识别其中的中文（含下面的中文文件名）。
rem  程序自身的输出已由 SetConsoleOutputCP(65001) 设为 UTF-8。
rem ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo   校园一卡通
echo ============================================================
echo.

set "GCC="
where gcc >nul 2>nul && set "GCC=gcc"
if not defined GCC if exist "C:\Program Files\mingw64\bin\gcc.exe" set "GCC=C:\Program Files\mingw64\bin\gcc.exe"
if not defined GCC if exist "C:\mingw64\bin\gcc.exe" set "GCC=C:\mingw64\bin\gcc.exe"

set "APP=校园一卡通.exe"

if exist "server.exe" if exist "%APP%" goto RUN
if not defined GCC goto NOGCC

echo [1/2] 正在编译（源码是 UTF-8，必须带 -fexec-charset=UTF-8）...
"%GCC%" -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 -Isrc src\server.c src\campus.c -o server.exe -lws2_32
if errorlevel 1 goto BUILDFAIL
"%GCC%" -std=c11 -Wall -Wextra -O2 -fexec-charset=UTF-8 -Isrc main.c src\campus.c -o main.exe
if errorlevel 1 goto BUILDFAIL
if exist campus.ico where windres >nul 2>nul && windres --input src\launcher.rc --output launcher_res.o --output-format=coff
"%GCC%" -std=c11 -Wall -Wextra -O2 -mwindows -fexec-charset=UTF-8 src\launcher.c launcher_res.o -o "%APP%" -lcomctl32 -lgdi32 -luser32 -lws2_32 -lshell32
if errorlevel 1 goto BUILDFAIL
if exist launcher_res.o del launcher_res.o
echo       编译完成。
echo.
goto RUN

:NOGCC
echo [错误] 没有找到 gcc，且程序尚未编译。
echo        请安装 MinGW-w64 后重试，或把编译好的 exe 放到本目录。
goto END

:BUILDFAIL
echo.
echo [错误] 编译失败，请查看上面的报错信息。
goto END

:RUN
echo [2/2] 正在启动桌面应用 ...
echo.
echo   如果没有出现窗口，可以单独启动后端：
echo       server.exe 8090 web data 0.0.0.0
echo   然后用浏览器打开   http://127.0.0.1:8090/
echo.
echo   演示账号
echo       学生     C001 / C002 / C003      登录密码 123456
echo       交易密码 010101 / 010102 / 010103
echo       管理员   admin123
echo.

start "" "%APP%"

:END
echo.
pause
