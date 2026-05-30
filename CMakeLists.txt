# CMake 3.20 is required for proper C++23 support
cmake_minimum_required(VERSION 3.20)

set(LIBNAME libretrocore)
project(${LIBNAME} LANGUAGES C CXX VERSION 1.0.1 DESCRIPTION "Libretro C++ Core Template")

# Default to Release build for maximum emulator performance
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

# Modern C++ settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Disable compiler-specific extensions (e.g., gnu++23)

# Keep the library size small by only exporting RETRO_API functions (Linux/macOS)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

# Remove the "lib" prefix so the output is "mycore_libretro.so/dll"
set(CMAKE_SHARED_LIBRARY_PREFIX "")

# MUST be ON when linking a STATIC library into a SHARED library
#set_target_properties(lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(${LIBNAME} SHARED src/libretro.cpp)

# Embed MinGW standard libraries so Windows DLLs are completely portable
if(MINGW)
    target_link_options(${LIBNAME} PRIVATE "-static-libgcc" "-static-libstdc++" "-Wl,-Bstatic" "-lstdc++" "-lpthread" "-Wl,-Bdynamic")
endif()

# In-source build output directories
set_target_properties(${LIBNAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin" # For Windows .dll
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin" # For Linux/macOS .so/.dylib
)

#target_link_libraries(${LIBNAME} PRIVATE lib)
target_include_directories(${LIBNAME} PRIVATE src)