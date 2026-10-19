@echo off
rem Builds libdolphinrvz (dolphinrvz.dll + drvz-cli) for one Windows architecture
rem and runs the tests.   scripts\build-win.cmd [x64|arm64]
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
set HERE=%~dp0..\
set BUILD=%HERE%build-%ARCH%
rem Next to NoIntrovert's other native libraries (native\out\<rid>).
set OUT=%HERE%..\out\win-%ARCH%

if "%ARCH%"=="x64" (set VCARCH=x64) else (set VCARCH=x64_arm64)
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul || exit /b 1

cmake -S "%HERE%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%BUILD%" || exit /b 1
if "%ARCH%"=="x64" (ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1)

if not exist "%OUT%" mkdir "%OUT%"
copy /y "%BUILD%\dolphinrvz.dll" "%OUT%\" >nul || exit /b 1
copy /y "%BUILD%\drvz-cli.exe" "%OUT%\" >nul || exit /b 1
echo Built %OUT%\dolphinrvz.dll
