@echo off
rem ============================================================
rem  SysClean build script
rem  Produces both x64 and x86 executables.
rem
rem  Requires: Visual Studio 2019/2022/2026 with
rem            "Desktop development with C++" workload
rem            + Windows 10/11 SDK
rem
rem  Usage:  double-click, or run build_exe.bat from a shell
rem  Output: ..\dist\SysClean.exe       (64-bit, recommended)
rem          ..\dist\SysClean_x86.exe   (32-bit, legacy systems)
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ---------- locate Visual Studio ----------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found.
    echo         Install Visual Studio 2019/2022/2026 with the
    echo         "Desktop development with C++" workload.
    exit /b 1
)

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [ERROR] Visual Studio found, but no C++ toolchain in it.
    echo         Open Visual Studio Installer and add
    echo         "Desktop development with C++".
    exit /b 1
)

rem ---------- newest MSVC toolset ----------
set "MSVCVER="
for /f "delims=" %%i in ('dir /b /ad /o-n "%VSDIR%\VC\Tools\MSVC" 2^>nul') do if not defined MSVCVER set "MSVCVER=%%i"
if not defined MSVCVER (
    echo [ERROR] No compiler under "%VSDIR%\VC\Tools\MSVC".
    exit /b 1
)
set "MSVC=%VSDIR%\VC\Tools\MSVC\%MSVCVER%"

rem ---------- newest Windows SDK ----------
set "SDK=%ProgramFiles(x86)%\Windows Kits\10"
if not exist "%SDK%\Include" set "SDK=%ProgramFiles%\Windows Kits\10"
if not exist "%SDK%\Include" (
    echo [ERROR] Windows SDK not found. Install Windows 10 or 11 SDK.
    exit /b 1
)
set "SDKV="
for /f "delims=" %%i in ('dir /b /ad /o-n "%SDK%\Include" 2^>nul') do if not defined SDKV set "SDKV=%%i"
if not defined SDKV (
    echo [ERROR] No SDK version folder under "%SDK%\Include".
    exit /b 1
)

echo ============================================================
echo   Visual Studio : %VSDIR%
echo   MSVC toolset  : %MSVCVER%
echo   Windows SDK   : %SDKV%
echo ============================================================
echo.

if not exist "..\dist" mkdir "..\dist"

set "CFLAGS=/nologo /MT /O2 /W3 /utf-8 /D_WIN32_WINNT=0x0601"
set "LIBS=user32.lib advapi32.lib comctl32.lib shlwapi.lib gdi32.lib shell32.lib psapi.lib"

rem ---------- x64 ----------
echo === [1/2] building x64  -^>  dist\SysClean.exe ===
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared"
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"
cl %CFLAGS% sysclean.c /link /SUBSYSTEM:WINDOWS /OUT:..\dist\SysClean.exe %LIBS% /MANIFEST:EMBED /MANIFESTINPUT:app.manifest /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
if errorlevel 1 goto FAIL
echo.

rem ---------- x86 ----------
echo === [2/2] building x86  -^>  dist\SysClean_x86.exe ===
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared"
set "PATH=%MSVC%\bin\Hostx64\x86;%SDK%\bin\%SDKV%\x86;%PATH%"
set "LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKV%\ucrt\x86;%SDK%\Lib\%SDKV%\um\x86"
cl %CFLAGS% sysclean.c /link /SUBSYSTEM:WINDOWS /OUT:..\dist\SysClean_x86.exe %LIBS% /MANIFEST:EMBED /MANIFESTINPUT:app.manifest /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
if errorlevel 1 goto FAIL

echo.
echo ============================================================
echo   BUILD OK
echo     ..\dist\SysClean.exe       (64-bit)
echo     ..\dist\SysClean_x86.exe   (32-bit)
echo ============================================================
goto DONE

:FAIL
echo.
echo ============================================================
echo   BUILD FAILED  (see compiler errors above)
echo ============================================================

:DONE
del /q *.obj 2>nul
endlocal
