cmake_minimum_required(VERSION 3.16)
project(CursorUsageWidget LANGUAGES CXX)

if(MINGW)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CURSOR_USAGE_SOURCES
  src/main.cpp
  src/util.cpp
  src/config.cpp
  src/auth.cpp
  src/http.cpp
  src/usage.cpp
  src/widget.cpp
)

# MSVC embeds app.manifest through the linker; MinGW needs it compiled in as a
# resource instead.
if(MINGW)
  list(APPEND CURSOR_USAGE_SOURCES src/app.rc)
  # windres does not report the included manifest as a dependency, so editing
  # app.manifest alone would otherwise link a stale one.
  set_property(SOURCE src/app.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()

add_executable(CursorUsage WIN32 ${CURSOR_USAGE_SOURCES})
set_target_properties(CursorUsage PROPERTIES OUTPUT_NAME "AI-Usage-Extension")

target_include_directories(CursorUsage PRIVATE src)

target_compile_definitions(CursorUsage PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)

target_link_libraries(CursorUsage PRIVATE
  winhttp
  advapi32
  gdiplus
  shell32
  ole32
  comctl32
  dwmapi
  uxtheme
  gdi32
  user32
)

if(MSVC)
  target_compile_options(CursorUsage PRIVATE /W4 /permissive-)
  target_link_options(CursorUsage PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
  )
elseif(MINGW)
  target_compile_options(CursorUsage PRIVATE -Wall -Wextra)
  target_link_options(CursorUsage PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -mwindows
    -municode
  )
  # std::async/std::thread need the threading runtime pulled in explicitly with
  # the static winpthread setup used here.
  find_package(Threads REQUIRED)
  target_link_libraries(CursorUsage PRIVATE Threads::Threads)
endif()

install(TARGETS CursorUsage RUNTIME DESTINATION bin)
