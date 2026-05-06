cmake_minimum_required(VERSION 3.15)

project(godot-hap VERSION 1.0.0)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|aarch64")
    set(ARCH "arm64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH "x86_64")
else()
    set(ARCH "x86_32")
endif()

add_subdirectory("${PROJECT_SOURCE_DIR}/lib/godot-cpp")

set(SNAPPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SNAPPY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SNAPPY_INSTALL OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${PROJECT_SOURCE_DIR}/lib/snappy" EXCLUDE_FROM_ALL)

file(GLOB_RECURSE INCLUDES "src/*.hpp")
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_library(GodotHap SHARED
    ${SOURCES} ${INCLUDES}
    "${PROJECT_SOURCE_DIR}/lib/hap/source/hap.c"
)

set_target_properties(GodotHap PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "godot-hap.$<IF:$<PLATFORM_ID:Windows>,windows,$<IF:$<PLATFORM_ID:Darwin>,macos,linux>>.$<IF:$<CONFIG:Debug>,debug,release>.${ARCH}"
)

target_include_directories(GodotHap PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/lib/godot-cpp/gdextension
    ${PROJECT_SOURCE_DIR}/lib/godot-cpp/include
    ${PROJECT_SOURCE_DIR}/lib/godot-cpp/gen/include
    ${PROJECT_SOURCE_DIR}/lib/hap/source
    ${PROJECT_SOURCE_DIR}/lib/minimp4
    ${PROJECT_SOURCE_DIR}/lib/snappy
)

target_link_libraries(GodotHap PRIVATE godot::cpp snappy)

set_target_properties(GodotHap PROPERTIES
    SKIP_RPATH TRUE
    BUILD_RPATH_USE_ORIGIN TRUE
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH_USE_LINK_PATH FALSE)
if(APPLE)
    set_target_properties(GodotHap PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(GodotHap PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

install(TARGETS GodotHap
    RUNTIME DESTINATION "${PROJECT_SOURCE_DIR}/demo/addons/godot-hap/bin"
    LIBRARY DESTINATION "${PROJECT_SOURCE_DIR}/demo/addons/godot-hap/bin"
)
