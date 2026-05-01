cmake_minimum_required(VERSION 3.19)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed - do like `cmake -B build`.")
endif()

if(NOT DEFINED CMAKE_CONFIGURATION_TYPES AND NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type")
endif()

project(StudioBrightness++ LANGUAGES CXX)

add_executable(studio-brightness-plusplus WIN32
src/main.cpp
src/hid.cpp
src/Settings.cpp
src/LogWindow.cpp
src/TrayPopup.cpp
src/OSDWindow.cpp
src/Log.cpp
studio-brightness-plusplus.rc
)

target_compile_features(studio-brightness-plusplus PRIVATE cxx_std_20)

target_compile_definitions(studio-brightness-plusplus PRIVATE UNICODE _UNICODE)

target_include_directories(studio-brightness-plusplus PRIVATE include)

target_link_libraries(studio-brightness-plusplus PRIVATE hid setupapi shlwapi wbemuuid comctl32 User32 Shell32 Gdi32
sensorsapi ole32 Advapi32 gdiplus PortableDeviceGuids
)

if(MINGW)
  target_link_options(studio-brightness-plusplus PRIVATE -municode)
endif()

if(MSVC)
  target_compile_options(studio-brightness-plusplus PRIVATE /W4 /MP)

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

install(TARGETS studio-brightness-plusplus)

file(GENERATE OUTPUT .gitignore CONTENT "*")
