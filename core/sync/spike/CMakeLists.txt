# ---------------------------------------------------------------------------
# core/sync/spike — the Fugue CRDT CORRECTNESS SPIKE. NOT A PRODUCT TARGET.
#
# docs/adr/0002-phase0-signoff-decisions.md says the custom-CRDT decision
# "means Phase 13 needs an explicit correctness spike before it is considered
# de-risked". This is that spike, and it is kept structurally separate from the
# shipping core so that separation is enforced by the build graph rather than by
# a comment someone has to remember:
#
#  * The target is named uapp_sync_fugue_spike, aliased uapp::sync_fugue_spike,
#    and NOTHING links it except tests/sync_spike. In particular core/capi — the
#    composition root for the C ABI — does not, so no amount of accidental
#    #include can expose this code to a mobile binding.
#  * It is added from the top-level CMakeLists.txt under UAPP_BUILD_SYNC_SPIKE
#    (default: follows UAPP_BUILD_TESTS), NOT from core/CMakeLists.txt. core/ is
#    "the portable domain layer"; an experiment is not part of it.
#  * It links ONLY uapp::compiler_options. It deliberately does NOT link
#    uapp::core_common, so the spike cannot reach uapp::Status, ObjectId,
#    core/blocks or storage/sqlite, and "this is standalone" is a fact the
#    linker checks. That is also why it carries its own SpikeStatus — see
#    include/uapp/sync/spike/element.h for the reasoning.
#
# Same compiler posture as everything else (ADR-0001: no exceptions, no RTTI,
# -Wall -Wextra -Wpedantic -Wconversion -Werror). A spike that compiled under
# weaker rules than production would not tell us whether the algorithm is
# implementable under our actual constraints, which is half the question.
# ---------------------------------------------------------------------------

add_library(uapp_sync_fugue_spike STATIC
    src/fugue.cpp
    src/reference_model.cpp
    src/rga.cpp)
add_library(uapp::sync_fugue_spike ALIAS uapp_sync_fugue_spike)

target_include_directories(uapp_sync_fugue_spike PUBLIC include)
target_link_libraries(uapp_sync_fugue_spike PRIVATE uapp::compiler_options)
