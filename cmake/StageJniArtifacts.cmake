# Post-build JNI staging script.
#
# Invoked by the top-level CMakeLists through `add_custom_command(... POST_BUILD)`
# once the Android emulator image exists. Script mode is used rather than an
# in-process `file(COPY)` because a POST_BUILD command runs outside the
# directory scope that defined the targets, so the variables it needs are
# passed explicitly.
#
# Every JNI artifact ends up under:
#
#     <stage dir>/jniLibs/<abi>/
#
# which is exactly where Gradle looks for `src/main/jniLibs/<abi>/`, so the
# packaged APK picks up whatever this build produced without a second copy step.

if (NOT DEFINED POUND_STAGE_DIR)
    message(FATAL_ERROR "StageJniArtifacts.cmake requires -DPOUND_STAGE_DIR=<build directory>.")
endif ()

if (NOT DEFINED POUND_STAGE_SOURCE)
    message(FATAL_ERROR "StageJniArtifacts.cmake requires -DPOUND_STAGE_SOURCE=<path to link output>.")
endif ()

# The ABI is baked into the output path by the toolchain file, so it is derived
# here rather than passed, keeping the two in sync by construction.
if (NOT DEFINED POUND_STAGE_ABI)
    get_filename_component(abi "${POUND_STAGE_SOURCE}" DIRECTORY)
    get_filename_component(abi "${abi}" NAME)
    set(POUND_STAGE_ABI "${abi}")
endif ()

set(staging "${POUND_STAGE_DIR}/jniLibs/${POUND_STAGE_ABI}")

if (abi STREQUAL "")
    message(FATAL_ERROR
            "StageJniArtifacts.cmake could not derive the ABI from ${POUND_STAGE_SOURCE}. "
            "Pass -DPOUND_STAGE_ABI=<abi> explicitly.")
endif ()

file(MAKE_DIRECTORY "${staging}")

if (NOT EXISTS "${POUND_STAGE_SOURCE}")
    message(FATAL_ERROR "StageJniArtifacts.cmake: missing link output: ${POUND_STAGE_SOURCE}")
endif ()

get_filename_component(source_name "${POUND_STAGE_SOURCE}" NAME)

file(COPY "${POUND_STAGE_SOURCE}" DESTINATION "${staging}")

message(STATUS "[Pound] Staged ${POUND_STAGE_ABI}/${source_name}")

# Collect any sibling shared libraries so a statically linked dependency that
# still produced a .so (for example a future ARM64 LuaJIT build) travels with
# the emulator image instead of being discovered missing at runtime.
get_filename_component(source_dir "${POUND_STAGE_SOURCE}" DIRECTORY)

file(GLOB sibling_libs "${source_dir}/*.so" "${source_dir}/*.dylib")

foreach (lib IN LISTS sibling_libs)
    get_filename_component(lib_name "${lib}" NAME)
    file(COPY "${lib}" DESTINATION "${staging}")
    message(STATUS "[Pound] Staged ${POUND_STAGE_ABI}/${lib_name}")
endforeach ()

if (NOT sibling_libs)
    message(STATUS "[Pound] No additional shared libraries to stage.")
endif ()