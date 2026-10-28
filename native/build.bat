@echo off
rem Builds native\out\main.dll (WaCaNative, the C++ half of WaCa ModKit) against the UE4SS SDK.
rem Needs: Visual Studio's C++ tools, Rust (cargo: UE4SS's patternsleuth), and a RE-UE4SS checkout with submodules at
rem the shipped UE4SS commit - by default tools\bin\src\RE-UE4SS, or set WACA_UE4SS_DIR. The first build compiles
rem all of UE4SS (about 10 minutes); later builds only the mod.
setlocal
cd /d "%~dp0"
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul 2>&1 || (echo Visual Studio C++ tools not found & exit /b 1)
set PATH=%USERPROFILE%\.cargo\bin;%PATH%
set UE4SS_ARG=
if defined WACA_UE4SS_DIR set UE4SS_ARG=-DWACA_UE4SS_DIR=%WACA_UE4SS_DIR%
rem configure only for a new build tree (~18 s): after that Ninja re-runs it itself when a CMakeLists.txt changes
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64 %UE4SS_ARG% || exit /b 1
cmake --build build --target WaCaNative proxy || exit /b 1
if not exist out mkdir out
if exist build\main.dll (copy /y build\main.dll out\main.dll >nul) else for /r build %%f in (main.dll) do if exist "%%f" copy /y "%%f" out\main.dll >nul
echo built %~dp0out\main.dll
