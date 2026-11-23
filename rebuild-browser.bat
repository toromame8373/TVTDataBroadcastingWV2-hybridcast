@echo off
rem ============================================================
rem  OPTIONAL: rebuild the browser-side JavaScript (prebuilt\dist) from browser\src.
rem  You do NOT need this to build the plugin (build.bat uses prebuilt\dist as it is).
rem  Requirements: Node.js (npm). Run this only if you changed the TypeScript sources.
rem ============================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "ROOT=%~dp0"

echo.
echo [1/4] Checking Node.js ...
where node >nul 2>nul
if errorlevel 1 goto :no_node
where npm >nul 2>nul
if errorlevel 1 goto :no_node

echo [2/4] Installing npm packages (browser folder) ...
pushd "%ROOT%browser"
call npm install --no-audit --no-fund
if errorlevel 1 goto :fail_npm_install
popd

echo [3/4] Applying the modified web-bml files ...
if not exist "%ROOT%web-bml-files\dist\client\browser.js" goto :no_override
xcopy "%ROOT%web-bml-files\dist" "%ROOT%browser\node_modules\web-bml\dist" /E /I /Y /Q >nul
if errorlevel 1 goto :fail_override
findstr /c:"startAITControlledApp" "%ROOT%browser\node_modules\web-bml\dist\client\browser.js" >nul
if errorlevel 1 goto :fail_override
findstr /c:"AITTransportMethod" "%ROOT%browser\node_modules\web-bml\dist\client\browser.js" >nul
if errorlevel 1 goto :fail_override

echo [4/4] Building (webpack) and updating prebuilt\dist ...
pushd "%ROOT%browser"
call npm run build
if errorlevel 1 goto :fail_npm_build
popd
if not exist "%ROOT%browser\dist\hybridcast_api.js" goto :fail_npm_build_nopopd
copy /y "%ROOT%browser\dist\*.js" "%ROOT%prebuilt\dist\" >nul
if errorlevel 1 goto :fail_copy
copy /y "%ROOT%browser\dist\*.LICENSE.txt" "%ROOT%prebuilt\dist\" >nul
if errorlevel 1 goto :fail_copy
copy /y "%ROOT%browser\node_modules\web-bml-fonts\*.woff2" "%ROOT%prebuilt\dist\" >nul
if errorlevel 1 goto :fail_copy

echo.
echo ================= DONE =================
echo prebuilt\dist was updated. Run build.bat to build the plugin.
echo (The checksums in prebuilt\BUILD_INFO.txt are now out of date.)
goto :end

:no_node
echo ERROR: Node.js (node / npm) was not found. Install it from https://nodejs.org/ and run this file again.
goto :fail
:fail_npm_install
popd
echo ERROR: "npm install" failed. Check your network connection.
goto :fail
:no_override
echo ERROR: web-bml-files folder is missing. Keep this file together with the web-bml-files folder.
goto :fail
:fail_override
echo ERROR: Could not apply the modified web-bml files.
goto :fail
:fail_npm_build
popd
:fail_npm_build_nopopd
echo ERROR: "npm run build" failed (browser\dist\hybridcast_api.js was not produced).
goto :fail
:fail_copy
echo ERROR: Could not copy the built files into prebuilt\dist.
goto :fail

:fail
echo.
echo ================= FAILED =================
pause
endlocal
exit /b 1

:end
echo.
pause
endlocal
exit /b 0
