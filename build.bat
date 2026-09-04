@echo off
setlocal enabledelayedexpansion

set "PORTABLE=OFF"
set "ZOOM_LEVEL=Ultra"
set "INTERACTIVE=1"

rem -- CLI args --------------------------------------------------------
for %%a in (%*) do (
    set "INTERACTIVE=0"
    if /I "%%~a"=="-Portable"   set "PORTABLE=ON"
    if /I "%%~a"=="-P"          set "PORTABLE=ON"
    if /I "%%~a"=="-Normal"         set "ZOOM_LEVEL=Normal"
    if /I "%%~a"=="-N"              set "ZOOM_LEVEL=Normal"
    if /I "%%~a"=="-Extra"          set "ZOOM_LEVEL=Extra"
    if /I "%%~a"=="-T"              set "ZOOM_LEVEL=Extra"
    if /I "%%~a"=="-Super"          set "ZOOM_LEVEL=Super"
    if /I "%%~a"=="-S"              set "ZOOM_LEVEL=Super"
    if /I "%%~a"=="-Ultra"          set "ZOOM_LEVEL=Ultra"
    if /I "%%~a"=="-U"              set "ZOOM_LEVEL=Ultra"
    if /I "%%~a"=="-Hyper"          set "ZOOM_LEVEL=Hyper"
    if /I "%%~a"=="-H"              set "ZOOM_LEVEL=Hyper"
    if /I "%%~a"=="-Extreme"        set "ZOOM_LEVEL=Extreme"
    if /I "%%~a"=="-X"              set "ZOOM_LEVEL=Extreme"
    if /I "%%~a"=="-Unimaginable"   set "ZOOM_LEVEL=Unimaginable"
    if /I "%%~a"=="-I"              set "ZOOM_LEVEL=Unimaginable"
)

rem -- Compiler detection ----------------------------------------------
set "CC=" & set "CXX=" & set "COMPILER_KIND="
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
    if exist "%%d\mingw64\bin\g++.exe" (
        set "CC=%%d\mingw64\bin\gcc.exe"
        set "CXX=%%d\mingw64\bin\g++.exe"
        set "COMPILER_KIND=G++ (MinGW)"
    )
)
if "%CXX%"=="" (
    where g++.exe >nul 2>&1 && (
        for /f "delims=" %%f in ('where g++.exe 2^>nul') do set "CXX=%%f"
        for /f "delims=" %%f in ('where gcc.exe 2^>nul')  do set "CC=%%f"
        set "COMPILER_KIND=G++ (PATH)"
    )
)
if "%CXX%"=="" (
    where clang++.exe >nul 2>&1 && (
        for /f "delims=" %%f in ('where clang++.exe 2^>nul') do set "CXX=%%f"
        for /f "delims=" %%f in ('where clang.exe 2^>nul')   do set "CC=%%f"
        set "COMPILER_KIND=Clang++"
    )
)
if "%CXX%"=="" (
    echo ERROR: No compiler found.
    exit /b 1
)

rem -- Banner ----------------------------------------------------------
set "VERSION="
for /f "tokens=3" %%v in ('findstr /c:"project(ADOCAO VERSION" CMakeLists.txt') do set "VERSION=%%v"
if "%VERSION%"=="" set "VERSION=?"
echo(
echo ==============================================
echo   ADOCAO  v%VERSION%  --  Build Script
echo ==============================================
echo(
echo Compiler detected: %COMPILER_KIND%
echo   CC  = %CC%
echo   CXX = %CXX%

rem -- Interactive prompts (skipped in CLI mode) -----------------------
if "%INTERACTIVE%"=="0" goto :summary
echo(
echo Build options (press Enter for default):
set /p "PORT_IN=  Static-linked portable build? (y/N): "
if /I "!PORT_IN!"=="y" set "PORTABLE=ON"
echo   Zoom level: Normal(10) Extra(5) Super(2.5) Ultra(1^) Hyper(0.5^) Extreme(0.25^) Unimaginable(0.1^)
set /p "ZOOM_IN=  Level [Ultra]: "
if /I "!ZOOM_IN!"=="Normal"         set "ZOOM_LEVEL=Normal"
if /I "!ZOOM_IN!"=="N"              set "ZOOM_LEVEL=Normal"
if /I "!ZOOM_IN!"=="Extra"          set "ZOOM_LEVEL=Extra"
if /I "!ZOOM_IN!"=="T"              set "ZOOM_LEVEL=Extra"
if /I "!ZOOM_IN!"=="Super"          set "ZOOM_LEVEL=Super"
if /I "!ZOOM_IN!"=="S"              set "ZOOM_LEVEL=Super"
if /I "!ZOOM_IN!"=="Ultra"          set "ZOOM_LEVEL=Ultra"
if /I "!ZOOM_IN!"=="U"              set "ZOOM_LEVEL=Ultra"
if /I "!ZOOM_IN!"=="Hyper"          set "ZOOM_LEVEL=Hyper"
if /I "!ZOOM_IN!"=="H"              set "ZOOM_LEVEL=Hyper"
if /I "!ZOOM_IN!"=="Extreme"        set "ZOOM_LEVEL=Extreme"
if /I "!ZOOM_IN!"=="X"              set "ZOOM_LEVEL=Extreme"
if /I "!ZOOM_IN!"=="Unimaginable"   set "ZOOM_LEVEL=Unimaginable"
if /I "!ZOOM_IN!"=="I"              set "ZOOM_LEVEL=Unimaginable"

:summary
rem -- Summary ---------------------------------------------------------
echo(
echo Configuration:
echo   Portable:    %PORTABLE%
echo   Zoom Level:  %ZOOM_LEVEL%
if defined GENERATOR (echo   Generator:   %GENERATOR%) else (echo   Generator:   MinGW Makefiles)
echo(

rem -- CMake configure -------------------------------------------------
if not exist build mkdir build
cd build

set "GEN=%GENERATOR%"
if "%GEN%"=="" set "GEN=MinGW Makefiles"

echo Configuring...
cmake .. -G "%GEN%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER="%CC%" ^
    -DCMAKE_CXX_COMPILER="%CXX%" ^
    -DADOCAO_PORTABLE=%PORTABLE% ^
    -DADOCAO_ZOOM_LEVEL=%ZOOM_LEVEL%
if %ERRORLEVEL% NEQ 0 (echo CMake configure FAILED. & cd .. & exit /b 1)
echo   OK

rem -- CMake build -----------------------------------------------------
echo Building...
cmake --build . --parallel
if %ERRORLEVEL% NEQ 0 (taskkill /f /im adocao.exe >nul 2>&1 & cmake --build . --parallel)
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED.
    cd ..
    exit /b 1
)
cd ..

rem -- Done ------------------------------------------------------------
echo(
echo ==============================================
echo   Build successful!
echo ==============================================
echo(
echo   %cd%\build\ADOCAO.exe
echo(
endlocal
