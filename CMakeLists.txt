cmake_minimum_required(VERSION 3.20)
project(WaraxLauncher LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "Собирается только MSVC (Visual Studio 2022) под Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # /MT — без зависимости от vcredist

set(WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2" CACHE PATH "Путь к NuGet-пакету Microsoft.Web.WebView2")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
  message(FATAL_ERROR "WebView2 SDK не найден: выполни  nuget install Microsoft.Web.WebView2 -OutputDirectory packages -ExcludeVersion")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Шифрование интерфейса и секретов -> src/secrets.h, src/web_pack.h (при каждой сборке)
file(GLOB WEB_SOURCES "${CMAKE_SOURCE_DIR}/web/*")
add_custom_command(
  OUTPUT "${CMAKE_SOURCE_DIR}/src/secrets.h" "${CMAKE_SOURCE_DIR}/src/web_pack.h"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/pack.py"
  DEPENDS ${WEB_SOURCES} "${CMAKE_SOURCE_DIR}/tools/pack.py"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Шифрую интерфейс и ключи...")

add_executable(WaraxLauncher WIN32
  src/main.cpp
  src/resource.rc
  src/app.manifest
  "${CMAKE_SOURCE_DIR}/src/secrets.h"
  "${CMAKE_SOURCE_DIR}/src/web_pack.h")

target_include_directories(WaraxLauncher PRIVATE src "${WEBVIEW2_DIR}/build/native/include")
target_compile_definitions(WaraxLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(WaraxLauncher PRIVATE /utf-8 /W3 /EHsc /permissive- /GS /guard:cf
  $<$<CONFIG:Release>:/O2 /GL /Gy>)
target_link_options(WaraxLauncher PRIVATE /DYNAMICBASE /NXCOMPAT /guard:cf
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /DEBUG:NONE>)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WV2_ARCH x64)
else()
  set(WV2_ARCH x86)
endif()
target_link_libraries(WaraxLauncher PRIVATE
  "${WEBVIEW2_DIR}/build/native/${WV2_ARCH}/WebView2LoaderStatic.lib"
  windowsapp bcrypt winhttp dwmapi shlwapi crypt32 ole32 oleaut32 shell32 advapi32 user32 gdi32 version)

option(WL_DEV "Режим разработки: выключает защиту (DevTools, отладчик)" OFF)
if(WL_DEV)
  target_compile_definitions(WaraxLauncher PRIVATE WL_DEV=1)
endif()
