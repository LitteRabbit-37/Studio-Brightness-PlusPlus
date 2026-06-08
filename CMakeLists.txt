cmake_minimum_required(VERSION 3.26)
project(StudioBrightnessPlusPlus)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Compiler options (MSVC) ----
if(MSVC)
    # Parallel compilation
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    # Resource compiler: find include/resource.h
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_CURRENT_SOURCE_DIR}/include\"")
endif()

add_compile_options(
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
)

add_compile_definitions(
        UNICODE
        _UNICODE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        # WIN32_LEAN_AND_MEAN and NOMINMAX are defined per-file in sources that need them
        # (e.g. main.cpp). Defining them globally strips COM/OLE types from windows.h,
        # which breaks GDI+ headers (GdiplusImaging.h needs IStream etc.).
)

# ---- Sources ----
set(SOURCES
        src/main.cpp
        src/hid.cpp
        src/Settings.cpp
        src/OSDWindow.cpp
        src/TrayPopup.cpp
        src/Log.cpp
        src/LogWindow.cpp
        studio-brightness-plusplus.rc   # compiled by rc.exe, icon + dialog resources
)

# WIN32 = use WinMain entry point (no console window)
add_executable(StudioBrightnessPlusPlus WIN32 ${SOURCES})

target_include_directories(StudioBrightnessPlusPlus PRIVATE include)

target_link_libraries(StudioBrightnessPlusPlus PRIVATE
        hid
        setupapi
        shlwapi
        wbemuuid
        comctl32
        user32
        shell32
        gdi32
        sensorsapi
        ole32
        advapi32
        gdiplus
        PortableDeviceGuids
)

# ---- Embed application manifest ----
if(MSVC)
    target_link_options(StudioBrightnessPlusPlus PRIVATE
            /MANIFEST:EMBED
            /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/studio-brightness-plusplus.manifest
    )
endif()
