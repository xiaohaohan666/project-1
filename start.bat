@echo off
rem ============================================================
rem  校园一卡通 —— 一键启动
rem
rem  本文件以 GBK 保存、行尾为 CRLF：
rem    · cmd 按系统 ANSI 代码页读取批处理，存成 UTF-8 会解析失败
rem    · 行尾必须是 CRLF；用 LF 时 rem 注释行会被截断，残句被当成命令
rem  程序自身的输出由 SetConsoleOutputCP(65001) 设为 UTF-8。
rem ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo   校园一卡通
echo ============================================================
echo.

set "APP=校园一卡通.exe"

rem ---------- 1. 缺什么编什么 ----------
if not exist "server.exe" goto BUILD
if not exist "%APP%"      goto BUILD
goto RUN

:BUILD
if not exist "build.exe" goto NOBUILD
echo [1/2] 正在构建（生成图标 + 编译程序 + 跑单元测试）...
echo.
build.exe
if errorlevel 1 goto BUILDFAIL
echo.
goto RUN

:NOBUILD
echo [错误] 找不到 build.exe，且程序尚未编译。
echo.
echo  请先编译构建器（需要 MinGW-w64 的 gcc）：
echo      gcc -std=c11 -O2 build.c -o build.exe -lz
echo  然后再运行本脚本。
goto END

:BUILDFAIL
echo.
echo [错误] 构建失败，请查看上面的报错信息。
goto END

:RUN
echo [2/2] 正在启动桌面应用 ...
echo.
echo   如果没有出现窗口，可单独启动后端：
echo       server.exe 8090 . data 0.0.0.0
echo   再用浏览器打开   http://127.0.0.1:8090/
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
