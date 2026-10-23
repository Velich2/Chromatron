@echo off
rem Chromatron - Windows build: After Effects (.aex) + OpenFX for Resolve (.ofx.bundle).
rem
rem   build_windows.bat            configure, build Release, run tests
rem   build_windows.bat install    same, then install the plug-ins (run as administrator)
rem
rem Override the SDK location without editing:  set AE_SDK_ROOT=D:\path\to\sdk

setlocal EnableDelayedExpansion

if not defined AE_SDK_ROOT set "AE_SDK_ROOT=C:\Users\Velich\AfterEffectsSDK_26.5_win"
set "BUILD_DIR=%~dp0build"

rem --- After Effects SDK: find the folder that contains Examples\Headers\AE_Effect.h ---
set "AE_SDK_DIR="
for /f "delims=" %%f in ('dir /s /b "%AE_SDK_ROOT%\AE_Effect.h" 2^>nul') do (
    if not defined AE_SDK_DIR (
        set "HEADER=%%f"
        set "CANDIDATE=!HEADER:\Examples\Headers\AE_Effect.h=!"
        if not "!CANDIDATE!"=="!HEADER!" set "AE_SDK_DIR=!CANDIDATE!"
    )
)
if not defined AE_SDK_DIR (
    echo ERROR: AE_Effect.h not found under "%AE_SDK_ROOT%".
    echo Set AE_SDK_ROOT to the unpacked After Effects SDK folder.
    exit /b 1
)
echo After Effects SDK: %AE_SDK_DIR%

rem --- Visual Studio environment (cl.exe, cmake) ---
where cl >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo ERROR: Visual Studio 2022 with "Desktop development with C++" is required.
        exit /b 1
    )
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
    if not defined VS_PATH (
        echo ERROR: no Visual Studio installation with the C++ toolset was found.
        exit /b 1
    )
    call "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)
where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: cmake not found. Install CMake or the Visual Studio CMake component.
    exit /b 1
)

rem --- Configure, build, test ---
cmake -S "%~dp0." -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 ^
    -DCHROMATRON_BUILD_AE=ON -DAE_SDK_DIR="%AE_SDK_DIR%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 exit /b 1

ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
if errorlevel 1 exit /b 1

echo.
echo After Effects: %BUILD_DIR%\AfterEffects\Chromatron.aex
echo Resolve (OFX): %BUILD_DIR%\Chromatron.ofx.bundle

if /i "%~1"=="install" (
    cmake --install "%BUILD_DIR%" --config Release
    if errorlevel 1 (
        echo ERROR: install failed - run this script as administrator.
        exit /b 1
    )
)
exit /b 0
