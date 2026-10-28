@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build every supported RP2350 configuration of pico-j2me (Windows).
rem The Pico SDK comes from PICO_SDK_PATH or the Raspberry Pi Pico VS Code
rem extension (%USERPROFILE%\.pico-sdk), as for a normal build.  A JDK 8 in
rem JAVA8_HOME rebuilds the class library, otherwise the prebuilt
rem classlib\classlib.jar is used.  The UF2 files land in bin\Release\.

set "ROOT=%~dp0"
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
cd /d "%ROOT%"

set "BUILD_TYPE=Release"
set "BUILD_ROOT=%ROOT%build\all"

rem Locate CMake.
set "CMAKE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
if not defined CMAKE if exist "%USERPROFILE%\.pico-sdk" (
    for /f "delims=" %%I in ('where /r "%USERPROFILE%\.pico-sdk" cmake.exe 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
)
if not defined CMAKE (
    echo ERROR: cmake.exe not found. 1>&2
    exit /b 1
)

rem Locate Ninja.
set "NINJA="
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%I"
if not defined NINJA if exist "%USERPROFILE%\.pico-sdk" (
    for /f "delims=" %%I in ('where /r "%USERPROFILE%\.pico-sdk" ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%I"
)
if not defined NINJA (
    echo ERROR: ninja.exe not found in PATH or "%USERPROFILE%\.pico-sdk". 1>&2
    exit /b 1
)

echo CMake: %CMAKE%
echo Ninja: %NINJA%
echo Source: %SRC%

rem board, video outputs, sound outputs - as wired on the board
for %%V in (VGA HDMI) do for %%A in (PWM I2S) do (
    call :build_one murmulator %%V %%A || exit /b 1
)
for %%V in (VGA HDMI) do for %%A in (PWM I2S) do (
    call :build_one murmulator2 %%V %%A || exit /b 1
)
call :build_one olimex-pico-pc HDMI PWM || exit /b 1
call :build_one waveshare_rp2350_pizero HDMI I2S || exit /b 1

echo.
echo All variants built.
echo UF2 files are under bin\%BUILD_TYPE%\.
exit /b 0

:build_one
set "BOARD=%~1"
set "VIDEO=%~2"
set "SOUND=%~3"
set "TAG=%BOARD%-%VIDEO%-%SOUND%"
set "VGA=OFF"
set "HDMI=OFF"
set "I2S=OFF"
if "%VIDEO%"=="VGA" set "VGA=ON"
if "%VIDEO%"=="HDMI" set "HDMI=ON"
if "%SOUND%"=="I2S" set "I2S=ON"
echo.
echo [%TAG%]
"%CMAKE%" -S "%SRC%" -B "%BUILD_ROOT%\%TAG%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DPICO_BOARD=%BOARD% ^
    -DVGA=%VGA% -DHDMI=%HDMI% -DI2S=%I2S% -DI2S_CS4334=OFF >nul || exit /b 1
"%CMAKE%" --build "%BUILD_ROOT%\%TAG%" --parallel >nul || exit /b 1
exit /b 0
