@echo off
rem ============================================================
rem  TVTDataBroadcastingWV2 (Hybridcast build) - double-click to build
rem  Usage:  build.bat          -> x64 (default)
rem          build.bat x86      -> 32-bit
rem  Requirements: Visual Studio 2022 with "Desktop development with C++" (Node.js / npm are NOT needed)
rem  The browser-side JavaScript is already built: prebuilt\dist
rem  (to rebuild it from browser\src, run rebuild-browser.bat - that one needs Node.js)
rem  Output: out\Plugins\ (copy the contents into the TVTest Plugins folder)
rem  NOTE: commands are never chained on one line; every step is checked with errorlevel.
rem ============================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "ROOT=%~dp0"
set "PLAT=x64"
set "PLATDIR=x64\"
if /i "%~1"=="x86" set "PLAT=Win32"
if /i "%~1"=="x86" set "PLATDIR="
set "NUGET_URL=https://dist.nuget.org/win-x86-commandline/latest/nuget.exe"

echo.
echo [1/4] Checking the prebuilt browser files ...
if not exist "%ROOT%prebuilt\dist\TVTDataBroadcastingWV2.js" goto :no_prebuilt
if not exist "%ROOT%prebuilt\dist\hybridcast_api.js" goto :no_prebuilt
if not exist "%ROOT%prebuilt\dist\KosugiMaru-Regular.woff2" goto :no_prebuilt
if not exist "%ROOT%browser\TVTDataBroadcastingWV2.html" goto :no_prebuilt

echo [2/4] Looking for MSBuild ...
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :try_path_msbuild
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
:try_path_msbuild
if defined MSBUILD goto :have_msbuild
where msbuild >nul 2>nul
if errorlevel 1 goto :no_msbuild
for /f "delims=" %%i in ('where msbuild') do set "MSBUILD=%%i"
:have_msbuild
echo     MSBuild: %MSBUILD%

echo [3/4] Restoring NuGet packages ...
"%MSBUILD%" "%ROOT%TVTDataBroadcastingWV2.sln" -t:restore -p:RestorePackagesConfig=true -p:Configuration=Release -p:Platform=%PLAT% -v:minimal
if not errorlevel 1 goto :restore_done
echo     MSBuild restore failed. Trying nuget.exe ...
if exist "%ROOT%nuget.exe" goto :have_nuget
curl.exe -L -o "%ROOT%nuget.exe" "%NUGET_URL%"
if errorlevel 1 goto :fail_nuget
:have_nuget
"%ROOT%nuget.exe" restore "%ROOT%TVTDataBroadcastingWV2.sln"
if errorlevel 1 goto :fail_nuget
:restore_done

echo [4/4] Building the plugin (Release, %PLAT%) ...
"%MSBUILD%" "%ROOT%TVTDataBroadcastingWV2.sln" -m -p:Configuration=Release -p:Platform=%PLAT% -v:minimal
if errorlevel 1 goto :fail_msbuild
set "DLL=%ROOT%%PLATDIR%Release\TVTDataBroadcastingWV2.tvtp"
if not exist "%DLL%" goto :fail_dll

echo.
echo Collecting files into out\Plugins ...
set "OUT=%ROOT%out\Plugins"
if exist "%ROOT%out" rmdir /s /q "%ROOT%out"
mkdir "%OUT%\TVTDataBroadcastingWV2\resources\dist"
if errorlevel 1 goto :fail_collect
copy /y "%DLL%" "%OUT%\" >nul
if errorlevel 1 goto :fail_collect
copy /y "%ROOT%browser\TVTDataBroadcastingWV2.html" "%OUT%\TVTDataBroadcastingWV2\resources\" >nul
if errorlevel 1 goto :fail_collect
xcopy "%ROOT%prebuilt\dist" "%OUT%\TVTDataBroadcastingWV2\resources\dist" /E /I /Y /Q >nul
if errorlevel 1 goto :fail_collect
if not exist "%ROOT%TVTDataBroadcastingWV2.ini.sample" goto :collect_done
copy /y "%ROOT%TVTDataBroadcastingWV2.ini.sample" "%OUT%\" >nul
if errorlevel 1 goto :fail_collect
:collect_done
copy /y "%ROOT%prebuilt\THIRD_PARTY_NOTICES.txt" "%OUT%\TVTDataBroadcastingWV2\" >nul

echo.
echo ================= BUILD SUCCEEDED =================
echo Copy the contents of this folder into the TVTest "Plugins" folder (close TVTest first):
echo   %OUT%
echo Then copy TVTDataBroadcastingWV2.ini.sample to TVTDataBroadcastingWV2.ini (same folder as the .tvtp) and check EnableNetwork=1.
echo The Hybridcast log is written to Plugins\TVTDataBroadcastingWV2\hybridcast.log
goto :end

:no_prebuilt
echo ERROR: the prebuilt browser files are missing (prebuilt\dist). Extract the whole zip and run this file from that folder.
echo        (Or run rebuild-browser.bat to build them; that needs Node.js.)
goto :fail
:no_msbuild
echo ERROR: MSBuild was not found. Install Visual Studio 2022 (Desktop development with C++) and run this file again.
goto :fail
:fail_nuget
echo ERROR: NuGet package restore failed. Check your network connection.
goto :fail
:fail_msbuild
echo ERROR: The plugin build failed. See the messages above.
goto :fail
:fail_dll
echo ERROR: TVTDataBroadcastingWV2.tvtp was not found at: %DLL%
goto :fail
:fail_collect
echo ERROR: Could not collect the output files.
goto :fail

:fail
echo.
echo ================= BUILD FAILED =================
pause
endlocal
exit /b 1

:end
echo.
pause
endlocal
exit /b 0
