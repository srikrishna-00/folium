add_library(uapp_core_common STATIC
    src/object_id.cpp
    src/ports.cpp
    src/status.cpp)
add_library(uapp::core_common ALIAS uapp_core_common)

target_include_directories(uapp_core_common PUBLIC include)
target_link_libraries(uapp_core_common PRIVATE uapp::compiler_options)

# The warning/exception posture is part of this library's contract, not just
# an implementation detail, so -fno-exceptions/-fno-rtti propagate to consumers.
#
# Gated on COMPILE_LANGUAGE:CXX since core/capi introduced C translation units
# downstream: exceptions and RTTI are C++ concepts, and handing these flags to
# a C compiler is a warning that -Werror turns into a build failure.
target_compile_options(uapp_core_common PUBLIC
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-exceptions>
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:CXX>>:-fno-rtti>)
