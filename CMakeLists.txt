cmake_minimum_required(VERSION 3.14)
project(NanoLog VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(NANOLOG_BUILD_SAMPLE "Build sample application" ON)
option(NANOLOG_BUILD_TESTS "Build unit tests" OFF)
option(NANOLOG_BUILD_BENCHMARKS "Build benchmarks" OFF)

# Common compile options
add_compile_options(-g -O3)

# Add subdirectories
add_subdirectory(runtime)

if(NANOLOG_BUILD_SAMPLE)
    add_subdirectory(sample)
endif()

# Install targets
install(TARGETS NanoLog
    EXPORT NanoLogTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(FILES
    runtime/NanoLogCpp17.h
    runtime/NanoLog.h
    runtime/Config.h
    runtime/Common.h
    runtime/Packer.h
    runtime/Cycles.h
    runtime/Util.h
    runtime/Log.h
    runtime/RuntimeLogger.h
    runtime/Portability.h
    runtime/Fence.h
    runtime/TimeTrace.h
    DESTINATION include/NanoLog
)

# Export targets
install(EXPORT NanoLogTargets
    FILE NanoLogTargets.cmake
    NAMESPACE NanoLog::
    DESTINATION lib/cmake/NanoLog
)

# Create config file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/NanoLogConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_file(cmake/NanoLogConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/NanoLogConfig.cmake"
    @ONLY
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/NanoLogConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/NanoLogConfigVersion.cmake"
    DESTINATION lib/cmake/NanoLog
)
