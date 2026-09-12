@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
set "CMAKE_EXE="
set "BUILD_DIR=%~dp0.build"

if not exist "%VSWHERE%" goto :no_vs

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"

if not defined VSROOT goto :no_vs
if not exist "%VSROOT%\Common7\Tools\VsDevCmd.bat" goto :no_vs

call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 goto :vs_init_failed

for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"

if not defined CMAKE_EXE if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined CMAKE_EXE goto :no_cmake

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"

"%CMAKE_EXE%" -S "%~dp0." -B "%BUILD_DIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :build_failed

"%CMAKE_EXE%" --build "%BUILD_DIR%"
if errorlevel 1 goto :build_failed

for %%I in ("%~dp0..\..\bin\EnhancedNomad.dll") do set "OUTPUT_DLL=%%~fI"

if not exist "%OUTPUT_DLL%" goto :missing_output

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%BUILD_DIR%" goto :cleanup_failed

echo.
echo ============================================================
echo   EnhancedNomad.dll built successfully
echo   %OUTPUT_DLL%
echo ============================================================
echo.
pause
exit /b 0

:no_vs
echo.
echo ERROR: Visual Studio with Desktop development with C++ was not found.
echo Install the C++ workload in Visual Studio Installer and run this file again.
echo.
pause
exit /b 1

:vs_init_failed
echo.
echo ERROR: Visual Studio C++ environment initialization failed.
echo.
pause
exit /b 1

:no_cmake
echo.
echo ERROR: CMake was not found.
echo Install the CMake component in Visual Studio Installer and run this file again.
echo.
pause
exit /b 1

:build_failed
echo.
echo ERROR: EnhancedNomad build failed.
echo.
pause
exit /b 1

:cleanup_failed
echo.
echo ERROR: EnhancedNomad.dll was built, but the temporary .build directory could not be removed.
echo.
pause
exit /b 1

:missing_output
echo.
echo ERROR: Build finished, but EnhancedNomad.dll was not found in enhanced_nomad\bin.
echo.
pause
exit /b 1
