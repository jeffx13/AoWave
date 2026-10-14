@echo off
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"
set "DIR=build\%PRESET%"
if /i "%PRESET%"=="debug" set "DIR=build\Desktop_Qt_6_12_0_MSVC2022_64bit_Debug"
if not defined QT_DIR set "QT_DIR=C:\Qt\6.12.0\msvc2022_64"
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
rem vcvars repoints VCPKG_ROOT at Visual Studio's bundled vcpkg; keep the user's own.
set "USER_VCPKG_ROOT=%VCPKG_ROOT%"
if not defined VSCMD_VER call "%VCVARS%" >nul || exit /b 1
if defined USER_VCPKG_ROOT set "VCPKG_ROOT=%USER_VCPKG_ROOT%"
cd /d "%~dp0.."
if not exist "%DIR%\build.ninja" (
  if not exist build mkdir build
  cmake --preset %PRESET% -DCMAKE_PREFIX_PATH="%QT_DIR%" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "build\configure-%PRESET%.log" 2>&1 || (type "build\configure-%PRESET%.log" & exit /b 1)
  del "build\configure-%PRESET%.log"
)
shift
cmake --build "%DIR%" %1 %2 %3 %4 -- --quiet
exit /b %errorlevel%
