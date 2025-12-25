# NanoLog Runtime Library
set(NANOLOG_SOURCES
    Cycles.cc
    Util.cc
    Log.cc
    NanoLog.cc
    RuntimeLogger.cc
    TimeTrace.cc
    testHelper/GeneratedCode.cc
)

set(NANOLOG_HEADERS
    NanoLogCpp17.h
    NanoLog.h
    Config.h
    Common.h
    Packer.h
    Cycles.h
    Util.h
    Log.h
    RuntimeLogger.h
    Portability.h
    Fence.h
    TimeTrace.h
)

# Create the static library
add_library(NanoLog STATIC ${NANOLOG_SOURCES})

target_include_directories(NanoLog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/NanoLog>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/testHelper
)

target_link_libraries(NanoLog
    PUBLIC
        pthread
        rt
)

target_compile_options(NanoLog PRIVATE
    -Wformat=2
)

# Build decompressor
add_executable(decompressor
    LogDecompressor.cc
    Cycles.cc
    Util.cc
    Log.cc
    testHelper/GeneratedCode.cc
)

target_include_directories(decompressor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/testHelper
)

target_link_libraries(decompressor
    pthread
    rt
)

# Build NanoLogCpp17Test (unit test)
add_executable(NanoLogCpp17Test
    NanoLogCpp17Test.cc
    Log.cc
    NanoLog.cc
    RuntimeLogger.cc
    Cycles.cc
    Util.cc
    TimeTrace.cc
    testHelper/GeneratedCode.cc
)

target_include_directories(NanoLogCpp17Test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/testHelper
)

find_package(GTest REQUIRED)
target_link_libraries(NanoLogCpp17Test
    NanoLog
    GTest::GTest
    GTest::Main
    pthread
    rt
)

enable_testing()
add_test(NAME NanoLogCpp17Test COMMAND NanoLogCpp17Test)
