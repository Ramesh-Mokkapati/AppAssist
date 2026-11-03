# AppAssist - log root cause diagnostic tool (MFC GUI + CLI) with embedded llama.cpp.
#
#   powershell -File scripts\fetch_deps.ps1        # once: llama.cpp + nlohmann/json into third_party\
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# Opening build\AppAssist.sln (or this folder) in Visual Studio 2022 works as well. Our code is compiled
# as C++14 and uses only C++11 library features; llama.cpp builds itself as C++17 into static libs.
cmake_minimum_required(VERSION 3.21)
# The release workflow (.github/workflows/build.yml) passes the next tag's version and its run number;
# local builds keep these defaults.
set(APPASSIST_VERSION "1.0.0" CACHE STRING "AppAssist version, major.minor.patch")
set(APPASSIST_BUILD_NUMBER "0" CACHE STRING "Fourth part of the file version (the CI run number)")
project(AppAssist VERSION ${APPASSIST_VERSION} LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "AppAssist is a Windows MFC application: build it with Visual Studio 2022 (MSVC).")
endif()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt"
   OR NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(FATAL_ERROR "Dependencies missing: run  powershell -File scripts\\fetch_deps.ps1  first.")
endif()

# ---- llama.cpp as static libraries (llama.lib, ggml*.lib), CPU only, no extras ----
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP       OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL         OFF CACHE BOOL "" FORCE)
set(LLAMA_ALL_WARNINGS    OFF CACHE BOOL "" FORCE)
set(GGML_ALL_WARNINGS     OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP           OFF CACHE BOOL "" FORCE)   # no vcomp140.dll dependency
# Portable x64 build (any CPU since ~2013) instead of one tuned to the build machine.
set(GGML_NATIVE           OFF CACHE BOOL "" FORCE)
set(GGML_AVX              ON  CACHE BOOL "" FORCE)
set(GGML_AVX2             ON  CACHE BOOL "" FORCE)
set(GGML_FMA              ON  CACHE BOOL "" FORCE)
set(GGML_F16C             ON  CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

# Third-party code: we don't maintain it, so its compiler warnings (C4244 conversions, C4068 GCC
# pragmas, ...) are switched off for its libraries only. AppAssist's own code stays at /W4.
foreach(third_party_target llama ggml ggml-base ggml-cpu)
    if(TARGET ${third_party_target})
        # /W0 (not /w): CMake maps it onto the project's WarningLevel setting instead of adding a
        # second, conflicting flag (which itself warns: D9025 "overriding '/W1' with '/w'").
        # The MSVC standard headers raise their own warning level (#pragma warning(push,
        # _STL_WARNING_LEVEL), never below 3), so conversions llama.cpp triggers inside <memory> and
        # <utility> still warn under /W0. Disabling those specific warnings works there too.
        target_compile_options(${third_party_target} PRIVATE /W0 /wd4244 /wd4267 /wd4068)
    endif()
endforeach()
# src\llama.cpp and src\models\llama.cpp share a file name; the models are compiled as one unity
# file, so there is no real clash, but MSBuild warns (MSB8027). Report it as a message instead.
set_target_properties(llama PROPERTIES VS_GLOBAL_MSBuildWarningsAsMessages "MSB8027")

# ---- model catalog and task profiles: config/models.json, built in as the default ----
# The JSON is compiled into the programs and also copied next to them, where it can be edited
# without rebuilding. MSVC limits one string literal to 16 KB, so it is split into adjacent raw
# string literals of 8000 characters, which the compiler joins (up to 64 KB in all).
file(READ "${CMAKE_SOURCE_DIR}/config/models.json" MODELS_JSON_TEXT)
string(LENGTH "${MODELS_JSON_TEXT}" models_json_length)
set(MODELS_JSON "")
set(models_json_offset 0)
while(models_json_offset LESS models_json_length)
    string(SUBSTRING "${MODELS_JSON_TEXT}" ${models_json_offset} 8000 models_json_chunk)
    if(models_json_offset GREATER 0)
        string(APPEND MODELS_JSON ")appassist_json\"\n        R\"appassist_json(")
    endif()
    string(APPEND MODELS_JSON "${models_json_chunk}")
    math(EXPR models_json_offset "${models_json_offset} + 8000")
endwhile()
configure_file(src/ModelCatalogDefault.h.in "${CMAKE_BINARY_DIR}/generated/ModelCatalogDefault.h" @ONLY)
configure_file(src/Version.h.in "${CMAKE_BINARY_DIR}/generated/Version.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/models.json)

# ---- engine shared by GUI and CLI ----
add_library(appassist_core STATIC
    src/CoreEngine.h src/CoreEngine.cpp
    src/EmbeddedLlamaEngine.h src/EmbeddedLlamaEngine.cpp
    src/LocalLLMClient.h src/LocalLLMClient.cpp
    src/ModelAdvisor.h src/ModelAdvisor.cpp
    src/ModelDownloader.h src/ModelDownloader.cpp)
target_include_directories(appassist_core PUBLIC src third_party PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(appassist_core PUBLIC llama winhttp bcrypt)

# (No WIN32_LEAN_AND_MEAN here: MFC needs the full Windows headers; the engine sources set it themselves.)
set(APPASSIST_DEFINITIONS UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
foreach(target appassist_core)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_definitions(${target} PRIVATE ${APPASSIST_DEFINITIONS})
    target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc /permissive-)
endforeach()

# ---- AppAssistCli.exe (console) ----
add_executable(AppAssistCli src/main_cli.cpp)
target_link_libraries(AppAssistCli PRIVATE appassist_core)

# ---- AppAssist.exe (MFC dialog, MFC in a shared DLL) ----
set(CMAKE_MFC_FLAG 2)
add_executable(AppAssist WIN32
    src/framework.h src/resource.h
    src/AppAssistApp.h src/AppAssistApp.cpp
    src/LogRootCauseAnalyzerDlg.h src/LogRootCauseAnalyzerDlg.cpp
    src/ModelAdvisorDlg.h src/ModelAdvisorDlg.cpp
    src/AppAssist.rc src/AppAssist.manifest)
target_compile_definitions(AppAssist PRIVATE _AFXDLL)
target_link_libraries(AppAssist PRIVATE appassist_core)
target_link_options(AppAssist PRIVATE /ENTRY:wWinMainCRTStartup)

foreach(target AppAssistCli AppAssist)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")   # Version.h (also for the .rc)
    target_compile_definitions(${target} PRIVATE ${APPASSIST_DEFINITIONS})
    target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc /permissive-)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/config/models.json" "$<TARGET_FILE_DIR:${target}>/models.json")
endforeach()
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT AppAssist)
