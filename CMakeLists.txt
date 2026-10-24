cmake_minimum_required(VERSION 3.19)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed - do like `cmake -B build`.")
endif()

if(NOT DEFINED CMAKE_CONFIGURATION_TYPES AND NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type")
endif()

set(version_h ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h)
if(NOT EXISTS ${version_h})
  set(gen_ps1 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-version.ps1)
  execute_process(COMMAND pwsh -c ${gen_ps1}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  RESULT_VARIABLE _gen_version_result)
  if(NOT _gen_version_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate ${version_h} from ${gen_ps1}")
  endif()
endif()

# get SBPP_VER_MAJOR SBPP_VER_MINOR SBPP_VER_PATCH SBPP_VER_BUILD from version.h
file(READ ${version_h} version_h_content)
string(REGEX MATCH "#define SBPP_VER_MAJOR ([0-9]+)" _ ${version_h_content})
set(SBPP_VER_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define SBPP_VER_MINOR ([0-9]+)" _ ${version_h_content})
set(SBPP_VER_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define SBPP_VER_PATCH ([0-9]+)" _ ${version_h_content})
set(SBPP_VER_PATCH ${CMAKE_MATCH_1})
string(REGEX MATCH "#define SBPP_VER_BUILD ([0-9]+)" _ ${version_h_content})
set(SBPP_VER_BUILD ${CMAKE_MATCH_1})
string(REGEX MATCH "#define SBPP_VERSION_NARROW \"([^\"]+)\"" _ ${version_h_content})
set(SBPP_VERSION_NARROW ${CMAKE_MATCH_1})

project(StudioBrightness++ LANGUAGES CXX VERSION ${SBPP_VER_MAJOR}.${SBPP_VER_MINOR}.${SBPP_VER_PATCH}.${SBPP_VER_BUILD})

add_executable(studio-brightness-plusplus WIN32
src/main.cpp
src/hid.cpp
src/Settings.cpp
src/LogWindow.cpp
src/TrayPopup.cpp
src/OSDWindow.cpp
src/Log.cpp
src/HdrMonitor.cpp
src/NvHdr.cpp
src/Updater.cpp
src/PresetConfirm.cpp
studio-brightness-plusplus.rc
)

set_target_properties(studio-brightness-plusplus PROPERTIES
RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_BINARY_DIR}/bin
)

target_compile_features(studio-brightness-plusplus PRIVATE cxx_std_20)

target_compile_definitions(studio-brightness-plusplus PRIVATE UNICODE _UNICODE)

target_include_directories(studio-brightness-plusplus PRIVATE include)

target_link_libraries(studio-brightness-plusplus PRIVATE hid setupapi shlwapi wbemuuid comctl32 User32 Shell32 Gdi32
sensorsapi ole32 Advapi32 gdiplus PortableDeviceGuids winhttp runtimeobject dxgi
)

if(MINGW)
  target_link_options(studio-brightness-plusplus PRIVATE -municode)
endif()

if(MSVC)
  target_compile_options(studio-brightness-plusplus PRIVATE /W4 /utf-8)

  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_link_options(studio-brightness-plusplus PRIVATE /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/studio-brightness-plusplus.manifest)
  else()
    target_link_options(studio-brightness-plusplus PRIVATE /MANIFEST:NO)

    add_custom_command(TARGET studio-brightness-plusplus POST_BUILD
      COMMAND "${CMAKE_MT}" /nologo
        /manifest "${CMAKE_CURRENT_SOURCE_DIR}/studio-brightness-plusplus.manifest"
        "/outputresource:$<TARGET_FILE:studio-brightness-plusplus>\\;1"
      COMMENT "Embedding studio-brightness-plusplus.manifest"
      VERBATIM
    )
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(studio-brightness-plusplus PRIVATE -Wall)
endif()

install(TARGETS studio-brightness-plusplus RUNTIME DESTINATION .)

# UpgradeCode must stay fixed forever so each new MSI cleanly replaces the previous install
# (reused from installer/studio-brightness-plusplus.wxs).
set(CPACK_WIX_UPGRADE_GUID "AE421BD1-A6FD-4D6F-803B-70289E6848BC")
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/studio-brightness-plusplus.ico")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/installer/license.rtf")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus")
set(CPACK_PACKAGE_EXECUTABLES "studio-brightness-plusplus" "Studio Brightness++")
set(CPACK_CREATE_DESKTOP_LINKS "studio-brightness-plusplus")

set(CPACK_GENERATOR "WIX")

set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CPACK_PACKAGE_VENDOR "LitteRabbit-37")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Studio Brightness++")
set(CPACK_PACKAGE_FILE_NAME "studio-brightness-plusplus-${SBPP_VERSION_NARROW}")

include(CPack)

file(GENERATE OUTPUT .gitignore CONTENT "*")
