# C++ build of lok_viewer.dll (same C ABI as the Rust build in src/lib.rs).
#   cmake -S . -B build-cpp -G "Visual Studio 17 2022" -A x64
#   cmake --build build-cpp --config Release
# Output: build-cpp/Release/lok_viewer.dll (picked up by dotnet/LokViewer.NET/LokViewer.NET.csproj)
cmake_minimum_required(VERSION 3.20)
project(lok_viewer_cpp LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "lok_viewer.cpp supports Windows only.")
endif()

add_library(lok_viewer SHARED src/lok_viewer.cpp)
target_include_directories(lok_viewer PRIVATE include)
target_compile_features(lok_viewer PRIVATE cxx_std_17)
# Static CRT: the DLL needs no VC++ redistributable on the target machine (same as the Rust build).
set_property(TARGET lok_viewer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  target_compile_options(lok_viewer PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
