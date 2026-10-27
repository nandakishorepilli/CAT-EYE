@echo off
REM ============================================================
REM  CAT's eye  –  build script (MSVC Release x64)
REM  Run from any prompt; the script locates vswhere/MSVC itself.
REM ============================================================
setlocal EnableDelayedExpansion

set "SRC=%~dp0main.cpp"
set "OUT=%~dp0CATs_eye.exe"

REM ── 1. Find Visual Studio via vswhere ────────────────────────
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (
        `"%VSWHERE%" -latest -products * -requires Microsoft.VisualCpp.Tools.Host64 -property installationPath`
    ) do set "VS_PATH=%%I"
)

if defined VS_PATH (
    set "VCVARS=!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS!" (
        set "VCVARS=!VS_PATH!\VC\Auxiliary\Build\vcvarsamd64.bat"
    )
)

REM ── 2. Try a few known hard-coded fallback paths ─────────────
if not defined VCVARS (
    for %%V in (2022 2019 2017) do (
        for %%E in (Enterprise Professional Community BuildTools) do (
            set "TRY=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
            if exist "!TRY!" ( set "VCVARS=!TRY!" & goto :found )
            set "TRY=%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
            if exist "!TRY!" ( set "VCVARS=!TRY!" & goto :found )
        )
    )
)
:found

if not defined VCVARS (
    echo.
    echo  ERROR: MSVC not found.  Install "Desktop development with C++" workload
    echo         from https://visualstudio.microsoft.com/downloads/#build-tools
    echo.
    pause
    exit /b 1
)

echo [build] Using: %VCVARS%
call "%VCVARS%" >nul 2>&1

REM ── 3. Compile ───────────────────────────────────────────────
echo [build] Compiling %SRC% ...
cl /nologo /O1 /GL /GR- /MT ^
   "%SRC%" ^
   /Fe:"%OUT%" ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib gdiplus.lib pdh.lib dwmapi.lib

if %ERRORLEVEL% neq 0 (
    echo.
    echo  BUILD FAILED  (exit code %ERRORLEVEL%)
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo  BUILD OK  ^>  %OUT%
echo.
endlocal
