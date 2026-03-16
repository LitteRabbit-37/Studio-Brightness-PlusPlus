@echo off

:: Setup Visual Studio environment if not already set
if not defined INCLUDE (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    )
)
if not defined INCLUDE (
    echo ERROR: Visual Studio environment not found. Install VS Build Tools or run from Developer Command Prompt.
    exit /b 1
)

md bin 2>nul
md obj 2>nul

:: Unicode flags for wcscpy_s and Windows Wide-char (W-suffix) APIs
set CXXFLAGS=/utf-8 /DUNICODE /D_UNICODE -MD -O2 -W4 -Iinclude /std:c++20 /EHsc

:: Compile source files
cl %CXXFLAGS% -c -Foobj/main.obj src/main.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/hid.obj src/hid.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/Settings.obj src/Settings.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/OSDWindow.obj src/OSDWindow.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/TrayPopup.obj src/TrayPopup.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/Log.obj src/Log.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/LogWindow.obj src/LogWindow.cpp
if errorlevel 1 exit /b 1

:: Compile resources
rc -Iinclude -foobj/studio-brightness-plusplus.res studio-brightness-plusplus.rc
if errorlevel 1 exit /b 1

:: Link everything
cl -Fe./bin/studio-brightness-plusplus.exe obj/main.obj obj/hid.obj obj/Settings.obj obj/OSDWindow.obj obj/TrayPopup.obj obj/Log.obj obj/LogWindow.obj obj/studio-brightness-plusplus.res ^
    -link /MANIFEST:EMBED /MANIFESTINPUT:studio-brightness-plusplus.manifest ^
    hid.lib setupapi.lib shlwapi.lib wbemuuid.lib comctl32.lib User32.lib Shell32.lib Gdi32.lib ^
    sensorsapi.lib ole32.lib Advapi32.lib gdiplus.lib PortableDeviceGuids.lib
if errorlevel 1 exit /b 1

echo Build successful.
