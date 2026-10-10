@echo off
chcp 1251 >nul 2>&1
setlocal EnableExtensions EnableDelayedExpansion

REM =============================================================
REM  Local Windows port of .github/workflows/build.yml
REM
REM  Requires (on PATH or discoverable via vswhere):
REM    - Visual Studio 2019/2022 with the "Desktop development
REM      with C++" workload and the MSVC x86 toolset
REM    - CMake >= 3.15 (multi-target --target), Ninja
REM    - Optional: sccache on PATH  -> enables compiler caching
REM    - Optional: System32\tar.exe -> fast zip; falls back to
REM      PowerShell Compress-Archive
REM
REM  Usage:
REM    build.bat                 full build + package
REM    build.bat dev             build only, skip packaging
REM    build.bat gc              build only the csgo_gc target
REM    build.bat srv             build only the srcds target
REM
REM  Flags may be combined, e.g. "build.bat gc dev".
REM =============================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build"
set "RELEASE_DIR=%ROOT%\release"
set "OUTNAME=csgo_gc-windows-latest.zip"
set "OUTZIP=%ROOT%\%OUTNAME%"

if not defined NUMBER_OF_PROCESSORS set "NUMBER_OF_PROCESSORS=4"
set "JOBS=%NUMBER_OF_PROCESSORS%"

set "DO_PACKAGE=1"
set "TARGETS=csgo srcds csgo_gc"

REM ---------- Parse arguments ------------------------------------
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="dev"          (set "DO_PACKAGE=0"        & shift & goto :parse_args)
if /I "%~1"=="--no-package" (set "DO_PACKAGE=0"        & shift & goto :parse_args)
if /I "%~1"=="gc"           (set "TARGETS=csgo_gc"     & shift & goto :parse_args)
if /I "%~1"=="srv"          (set "TARGETS=srcds"       & shift & goto :parse_args)
echo [WARN] Unknown argument ignored: %~1
shift
goto :parse_args
:args_done

echo [*] Workspace:  %ROOT%
echo [*] Targets:    %TARGETS%
echo [*] Jobs:       %JOBS%
echo [*] Packaging:  %DO_PACKAGE%
echo Start time: %date% %time%

REM ---------- Locate Visual Studio --------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install VS 2019/2022 with C++ tools.
  exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSINSTALL=%%i"

if "%VSINSTALL%"=="" (
  echo [ERROR] No Visual Studio with VC.Tools.x86.x64 found.
  exit /b 1
)
echo [*] Visual Studio: %VSINSTALL%

REM ---------- Prefer VS-bundled CMake / Ninja ---------------------
set "VSCMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%VSCMAKE%\cmake.exe" set "PATH=%VSCMAKE%;%PATH%"
if exist "%VSNINJA%\ninja.exe" set "PATH=%VSNINJA%;%PATH%"

REM ---------- Initialize MSVC x86 (skip if already active) --------
if /I "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  echo [*] vcvars x86 already active - skipping
) else (
  call "%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat"
)
where cl    >nul 2>&1 || (echo [ERROR] cl.exe not on PATH after vcvars. & exit /b 1)
where cmake >nul 2>&1 || (echo [ERROR] cmake not found in PATH. & exit /b 1)
where ninja >nul 2>&1 || (echo [ERROR] ninja not found in PATH. & exit /b 1)

REM ---------- Detect optional sccache -----------------------------
set "LAUNCHER_OPTS="
where sccache >nul 2>&1
if not errorlevel 1 (
  echo [*] sccache found - enabling compiler cache
  set "LAUNCHER_OPTS=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache"
) else (
  echo [*] sccache not found - building without a compiler cache
)

REM ---------- Configure ------------------------------------------
echo [*] Configuring...
if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
cmake -G Ninja -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release ^
      %LAUNCHER_OPTS% ^
      -S "%ROOT%" -DOUTDIR="%RELEASE_DIR%"
if errorlevel 1 (echo [ERROR] cmake configure failed. & exit /b 1)

REM ---------- Build ----------------------------------------------
echo [*] Building targets: %TARGETS%  (%JOBS% parallel jobs)
taskkill /f /im csgo.exe
cmake --build "%BUILD_DIR%" --target %TARGETS% --parallel %JOBS%
if errorlevel 1 (echo [ERROR] cmake build failed. & exit /b 1)

REM ---------- Copy csgo_gc.dll to parent -----------------------
set "DLL_DEST=%~dp0..\csgo_gc.dll"
if exist "%RELEASE_DIR%\csgo_gc\csgo_gc.dll" (
  echo [*] Copying csgo_gc.dll to "%DLL_DEST%"
  copy /Y "%RELEASE_DIR%\csgo_gc\csgo_gc.dll" "%DLL_DEST%" >nul
  if errorlevel 1 (echo [ERROR] Failed to copy csgo_gc.dll. & exit /b 1)
) else (
  echo [WARN] "%RELEASE_DIR%\csgo_gc\csgo_gc.dll" not found - skipping parent copy.
)

if "%DO_PACKAGE%"=="0" (
  echo [*] Skipping packaging (dev mode)
  goto :done
)

REM ---------- Package --------------------------------------------
echo [*] Packaging...
copy /Y "%ROOT%\README.md" "%RELEASE_DIR%\" >nul

if not exist "%RELEASE_DIR%\licenses" mkdir "%RELEASE_DIR%\licenses"
if not exist "%RELEASE_DIR%\csgo_gc"  mkdir "%RELEASE_DIR%\csgo_gc"

copy /Y "%ROOT%\LICENSE" "%RELEASE_DIR%\licenses\LICENSE-csgo_gc.txt" >nul
copy /Y "%BUILD_DIR%\_deps\cryptopp-cmake-build\cryptopp\License.txt" ^
     "%RELEASE_DIR%\licenses\LICENSE-crypto++.txt" >nul
copy /Y "%BUILD_DIR%\_deps\distorm-src\COPYING" ^
     "%RELEASE_DIR%\licenses\LICENSE-distorm.txt" >nul
copy /Y "%BUILD_DIR%\_deps\funchook-src\LICENSE" ^
     "%RELEASE_DIR%\licenses\LICENSE-funchook.txt" >nul
copy /Y "%BUILD_DIR%\_deps\protobuf-src\LICENSE" ^
     "%RELEASE_DIR%\licenses\LICENSE-protobuf.txt" >nul

copy /Y "%ROOT%\examples\config.txt"             "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\inventory.txt"          "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\price_sheet.txt"        "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\unusual_loot_lists.txt" "%RELEASE_DIR%\csgo_gc\" >nul
copy /Y "%ROOT%\examples\passes.txt"             "%RELEASE_DIR%\csgo_gc\" >nul

REM ---------- Zip ------------------------------------------------
REM  Prefer System32\tar.exe (bsdtar) over Compress-Archive: ~5-10x
REM  faster on the same tree.  Note that entries will carry a "./"
REM  prefix; harmless for almost every consumer.
echo [*] Creating archive: %OUTZIP%
if exist "%OUTZIP%" del /Q "%OUTZIP%"

set "ZIP_DONE=0"
set "SYSTAR=%SystemRoot%\System32\tar.exe"
if exist "%SYSTAR%" (
  "%SYSTAR%" -a -c -f "%OUTZIP%" -C "%RELEASE_DIR%" .
  if not errorlevel 1 set "ZIP_DONE=1"
)

if "%ZIP_DONE%"=="1" goto :zip_done

echo [*] tar unavailable or failed - falling back to Compress-Archive
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%RELEASE_DIR%\*' -DestinationPath '%OUTZIP%' -Force"
if errorlevel 1 (echo [ERROR] Failed to create zip. & exit /b 1)

:zip_done
echo [OK] Done: %OUTZIP%

:done
echo End time: %date% %time%
endlocal
exit /b 0