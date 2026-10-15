@echo off
rem ============================================================
rem  校园一卡通（终端版）—— 运行
rem
rem  本文件以 GBK 保存、行尾为 CRLF：
rem    · cmd 按系统 ANSI 代码页读取批处理，存成 UTF-8 会解析失败
rem    · 行尾若为 LF，rem 注释行会被截断，残句被当成命令执行
rem ============================================================
setlocal
cd /d "%~dp0"

if exist "main.exe" goto RUN

echo 尚未编译，正在构建 ...
echo.
if not exist "build.exe" goto NOBUILD
build.exe
if errorlevel 1 goto FAIL
echo.

:RUN
main.exe
goto END

:NOBUILD
echo [错误] 找不到 build.exe，也没有 main.exe。
echo.
echo  请安装 MinGW-w64 后手工编译：
echo      gcc -std=c11 -O2 -fexec-charset=UTF-8 main.c campus.c -o main.exe
goto END

:FAIL
echo.
echo [错误] 构建失败，请查看上面的信息。

:END
echo.
pause
