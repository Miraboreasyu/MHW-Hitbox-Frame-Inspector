@echo off
:: ============================================================================
:: Build MHW Debug Hitbox Mod (Stracker's Loader plugin, DX11 + DX12)
:: Output: build\debug_hitbox.dll -> nativePC\plugins\debug_hitbox.dll
:: ============================================================================

setlocal

set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: Visual Studio not found at %VSDIR%
    exit /b 1
)

set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set "SRCDIR=%~dp0src"
set "OUTDIR=%~dp0build"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo ===== Building MHW Debug Hitbox Mod (15.23 DX11+DX12) =====
echo Source: %SRCDIR%
echo Output: %OUTDIR%
echo.

cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" ^
    /utf-8 ^
    /I "%SRCDIR%" ^
    "%SRCDIR%\dllmain.cpp" ^
    "%SRCDIR%\hook.cpp" ^
    "%SRCDIR%\log_config.cpp" ^
    "%SRCDIR%\game_freeze.cpp" ^
    "%SRCDIR%\collision_collector.cpp" ^
    "%SRCDIR%\overlay_renderer.cpp" ^
    /Fe:"%OUTDIR%\debug_hitbox.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib user32.lib uuid.lib /OPT:REF /OPT:ICF ^
    /OUT:"%OUTDIR%\debug_hitbox.dll"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ===== BUILD FAILED =====
    exit /b 1
)

echo.
echo ===== BUILD SUCCESS =====
echo Output: %OUTDIR%\debug_hitbox.dll
exit /b 0
