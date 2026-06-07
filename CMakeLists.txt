# CMake 3.20 is required for proper C++23 support
cmake_minimum_required(VERSION 3.20)

set(LIBNAME spheroid_libretro)
project(${LIBNAME} LANGUAGES C CXX VERSION 1.0.1 DESCRIPTION "Spheroid Libretro Core")

# Default to Release build for maximum emulator performance
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

# Modern C++ settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Disable compiler-specific extensions

# Keep the library size small by only exporting RETRO_API functions (Linux/macOS)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

# Remove the "lib" prefix so the output is "mycore_libretro.so/dll"
set(CMAKE_SHARED_LIBRARY_PREFIX "")

# ==========================================
# QuickJS Setup
# ==========================================
set(QJS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/quickjs")

# The core QuickJS source files required to build the engine
set(QJS_SOURCES
    ${QJS_DIR}/quickjs.c
    ${QJS_DIR}/libregexp.c
    ${QJS_DIR}/libunicode.c
    ${QJS_DIR}/cutils.c
	${QJS_DIR}/dtoa.c
    ${QJS_DIR}/quickjs-libc.c # Optional, provides basic file I/O and timers if needed
)

# QuickJS requires a version string
add_definitions(-DCONFIG_VERSION="2026-06-04") # Use the date of your QuickJS version

# ==========================================
# Core Build
# ==========================================

add_library(${LIBNAME} SHARED 
    src/libretro.cpp
    src/gpu.cpp
	src/script.cpp
    src/miniaudio.c
    ${QJS_SOURCES}
)

if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_target_properties(${LIBNAME} PROPERTIES LINK_FLAGS "-static -static-libgcc -static-libstdc++")
    
    # Also force statically linking the threading library
    target_link_libraries(${LIBNAME} PRIVATE "-Wl,-Bstatic" pthread "-Wl,-Bdynamic")
endif()

# In-source build output directories
set_target_properties(${LIBNAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin" # For Windows .dll
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin" # For Linux/macOS .so/.dylib
)

# QuickJS requires libm (math) and libdl (dynamic linking) on Linux/macOS
if (NOT WIN32)
    target_link_libraries(${LIBNAME} PRIVATE m dl pthread)
endif()

# Include directories
target_include_directories(${LIBNAME} PRIVATE src ${QJS_DIR})