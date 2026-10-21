@echo off
chcp 936 >nul 2>&1
setlocal enabledelayedexpansion
title 进程清理（保留系统必需进程）

rem ============================================================
rem  免编译版：只依赖系统自带的 tasklist / taskkill
rem  范围：当前用户会话内、不在白名单里的进程
rem  不动：Session 0 的系统服务、内核关键进程
rem  需要更精细的控制（预览/PID 明细/自定义白名单）请用 SysClean.exe
rem ============================================================

rem 白名单：两端留空格，用于整词匹配
set "WHITE= system.exe smss.exe csrss.exe wininit.exe winlogon.exe services.exe lsass.exe svchost.exe explorer.exe dwm.exe taskhost.exe taskhostw.exe conhost.exe audiodg.exe wmiprvse.exe dllhost.exe spoolsv.exe spoolss.exe searchindexer.exe taskeng.exe ctfmon.exe logonui.exe consent.exe sppsvc.exe msdtc.exe trustedinstaller.exe tiworker.exe wuauclt.exe wudfhost.exe sihost.exe runtimebroker.exe fontdrvhost.exe userinit.exe cmd.exe "

echo ==================================================
echo    进程清理（保留系统必需进程）  免编译版
echo ==================================================
echo.
echo  范围：当前用户会话内、不在白名单中的全部进程
echo  不动：Session 0 的系统服务、内核关键进程
echo.

set "LIST=%TEMP%\procsweep_%RANDOM%%RANDOM%.tmp"
type nul > "%LIST%"

echo  正在扫描进程...
echo.

for /f "tokens=1,2,4 delims=," %%a in ('tasklist /fo csv /nh 2^>nul') do (
    set "PNAME=%%~a"
    set "PPID=%%~b"
    set "PSESS=%%~c"
    if not "!PPID!"=="" if not "!PPID!"=="0" if not "!PPID!"=="4" if not "!PSESS!"=="0" (
        echo !WHITE! | findstr /i /c:" !PNAME! " >nul
        if errorlevel 1 (
            echo !PPID!^|!PNAME!>>"%LIST%"
        )
    )
)

set /a N=0
for /f "usebackq delims=" %%L in ("%LIST%") do set /a N+=1

if %N%==0 (
    echo  没有需要终止的进程，系统已经很干净。
    del "%LIST%" >nul 2>&1
    echo.
    pause
    exit /b 0
)

echo  [将终止] %N% 个进程：
echo.
for /f "usebackq tokens=1,2 delims=|" %%x in ("%LIST%") do (
    echo     PID %%~x    %%y
)

echo.
set "ANS="
set /p "ANS=  确认终止以上进程？输入 Y 回车继续（直接回车取消）: "
if /i not "%ANS%"=="Y" (
    echo.
    echo  已取消，未做任何操作。
    del "%LIST%" >nul 2>&1
    echo.
    pause
    exit /b 1
)

echo.
echo  执行中...
set /a OK=0
set /a FAIL=0
for /f "usebackq tokens=1,2 delims=|" %%x in ("%LIST%") do (
    taskkill /f /pid %%~x >nul 2>&1
    if errorlevel 1 (
        set /a FAIL+=1
    ) else (
        set /a OK+=1
    )
)

del "%LIST%" >nul 2>&1

echo.
echo ==================================================
echo    完成：成功 %OK% 个，失败 %FAIL% 个
echo ==================================================
echo.
echo  失败的一般是权限不足（系统进程）或进程已自行退出。
echo  想清得更彻底：右键本文件「以管理员身份运行」。
echo.
pause
endlocal
