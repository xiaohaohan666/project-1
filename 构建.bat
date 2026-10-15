@echo off
rem ============================================================
rem  校园一卡通（终端版）—— 构建
rem  生成 main.exe，并跑一遍单元测试。
rem  本文件以 GBK 保存、行尾为 CRLF（原因见 运行.bat 的注释）。
rem ============================================================
setlocal
cd /d "%~dp0"

if not exist "build.exe" goto NOBUILD

build.exe %*
goto END

:NOBUILD
echo [错误] 找不到 build.exe。
echo.
echo  请安装 MinGW-w64 后先编译构建器：
echo      gcc -std=c11 -O2 build.c -o build.exe
echo.
echo  也可以跳过构建器，直接编译主程序：
echo      gcc -std=c11 -O2 -fexec-charset=UTF-8 main.c campus.c -o main.exe

:END
echo.
pause
