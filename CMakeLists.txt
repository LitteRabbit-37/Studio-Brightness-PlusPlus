cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type")
endif()

project(StudioBrightness LANGUAGES CXX)

add_executable(studio-brightness-plusplus WIN32 src/main.cpp src/hid.cpp studio-brightness-plusplus.rc)

target_compile_features(studio-brightness-plusplus PRIVATE cxx_std_20)

target_include_directories(studio-brightness-plusplus PRIVATE include)

target_link_libraries(studio-brightness-plusplus PRIVATE hid setupapi wbemuuid comctl32 User32 Shell32 Gdi32)

target_link_options(studio-brightness-plusplus PRIVATE "$<$<BOOL:${MINGW}>:-municode>")

if(MSVC)
  target_compile_options(studio-brightness-plusplus PRIVATE /W4)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(studio-brightness-plusplus PRIVATE -Wall -Wold-style-cast)
endif()

install(TARGETS studio-brightness-plusplus)
