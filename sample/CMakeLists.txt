# Sample Application
add_executable(sampleApplication main.cc)

target_link_libraries(sampleApplication
    PRIVATE
        NanoLog
)

target_compile_options(sampleApplication PRIVATE
    -Werror=format
    -DNDEBUG
)
