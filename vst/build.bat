@echo off
rem ==========================================================================
rem  Builds The Ham VST3 + Standalone (Release) and the test tool.
rem  Usage:  build.bat [Debug|Release]
rem  Build output and the JUCE download go to %LOCALAPPDATA%\TheHamVST
rem  so OneDrive doesn't try to sync ~1 GB of intermediate files.
rem  An existing JUCE 8.0.17 download from Brass Knuckles or E-Piano is reused.
rem ==========================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "WORK=%LOCALAPPDATA%\TheHamVST"
set "BUILD=%WORK%\build"

rem --- find CMake (PATH first, then the copy bundled with Visual Studio) ---
set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

rem --- git is needed to fetch JUCE ---
where git >nul 2>nul
if errorlevel 1 if exist "%ProgramFiles%\Git\cmd\git.exe" set "PATH=%ProgramFiles%\Git\cmd;%PATH%"

rem --- reuse an existing JUCE checkout instead of downloading again ---
set "JUCE_ARG="
if not exist "%WORK%\deps\juce-src\CMakeLists.txt" (
  if exist "%LOCALAPPDATA%\BrassKnucklesVST\deps\juce-src\CMakeLists.txt" (
    set "JUCE_ARG=-DHAM_JUCE_DIR=%LOCALAPPDATA%\BrassKnucklesVST\deps\juce-src"
  ) else if exist "%LOCALAPPDATA%\EPianoVST\deps\juce-src\CMakeLists.txt" (
    set "JUCE_ARG=-DHAM_JUCE_DIR=%LOCALAPPDATA%\EPianoVST\deps\juce-src"
  )
)

echo [The Ham] configuring...
"%CMAKE%" -S "%~dp0." -B "%BUILD%" -DFETCHCONTENT_BASE_DIR="%WORK%\deps" %JUCE_ARG% || goto :fail

echo [The Ham] building %CONFIG%...
"%CMAKE%" --build "%BUILD%" --config %CONFIG% --target TheHamVST_VST3 TheHamVST_Standalone TheHamTests --parallel || goto :fail

echo.
echo [The Ham] Done.
echo   VST3       : %BUILD%\TheHamVST_artefacts\%CONFIG%\VST3\The Ham.vst3
echo   Standalone : %BUILD%\TheHamVST_artefacts\%CONFIG%\Standalone\The Ham.exe
echo   Tests      : %BUILD%\TheHamTests_artefacts\%CONFIG%\TheHamTests.exe
echo   Run install.bat to copy the VST3 into C:\Program Files\Common Files\VST3
exit /b 0

:fail
echo [The Ham] BUILD FAILED
exit /b 1
