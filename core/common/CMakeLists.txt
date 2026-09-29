add_library(uapp_core_common STATIC
    src/status.cpp)
add_library(uapp::core_common ALIAS uapp_core_common)

target_include_directories(uapp_core_common PUBLIC include)
target_link_libraries(uapp_core_common PRIVATE uapp::compiler_options)

# The warning/exception posture is part of this library's contract, not just
# an implementation detail, so -fno-exceptions/-fno-rtti propagate to consumers.
target_compile_options(uapp_core_common PUBLIC
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-exceptions -fno-rtti>)
