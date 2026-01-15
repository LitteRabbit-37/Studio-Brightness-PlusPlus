@echo off
md bin 2>nul
md obj 2>nul

:: Définitions UNICODE pour wcscpy_s et les API Windows Wide (W-suffix)
set CXXFLAGS=/DUNICODE /D_UNICODE -MD -O2 -W4 -Iinclude /std:c++20 /EHsc

:: Compile source files
cl %CXXFLAGS% -c -Foobj/main.obj src/main.cpp
if errorlevel 1 exit /b 1

cl %CXXFLAGS% -c -Foobj/hid.obj src/hid.cpp
if errorlevel 1 exit /b 1

:: Compile resources
rc -Iinclude -foobj/studio-brightness-plusplus.res studio-brightness-plusplus.rc
if errorlevel 1 exit /b 1

:: Link everything
cl -Fe./bin/studio-brightness-plusplus.exe obj/main.obj obj/hid.obj obj/studio-brightness-plusplus.res ^
    -link hid.lib setupapi.lib shlwapi.lib wbemuuid.lib comctl32.lib User32.lib Shell32.lib Gdi32.lib ^
    sensorsapi.lib ole32.lib Advapi32.lib

