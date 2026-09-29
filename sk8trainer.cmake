# SK8TRAINER CMake module for a rexglue Skate 3 recompilation.
#
#   include(<path to sk8trainer>/sk8trainer.cmake)
#   sk8trainer_add(skate3)        # after add_executable/add_library(skate3 ...)
#
# Adds the trainer sources to the target and defines SKATE3_TRAINER=1, which
# switches on the few wiring lines in the recomp (see INTEGRATION.md).
# Portable: the same sources build for Windows, Linux/Steam Deck, macOS,
# Android and iOS; nothing game-derived is compiled in.

set(SK8TRAINER_ROOT "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "SK8TRAINER source root")

function(sk8trainer_add target)
    target_sources(${target} PRIVATE
        "${SK8TRAINER_ROOT}/src/skate3_trainer.cpp"
        "${SK8TRAINER_ROOT}/src/skate3_trainer_practice.cpp"
        "${SK8TRAINER_ROOT}/src/skate3_trainer_vault.cpp"
    )
    target_include_directories(${target} PRIVATE "${SK8TRAINER_ROOT}/src")
    target_compile_definitions(${target} PRIVATE SKATE3_TRAINER=1)
endfunction()
