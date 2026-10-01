cmake_minimum_required(VERSION 3.25..3.31)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # Strict ISO C

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CmakePolicies)

project(Pound LANGUAGES C CXX VERSION 0.1.0)

include(CompilerFlags)
include(Platform)

# -----------------------------------------------------------------------------
# Platform and architecture
# -----------------------------------------------------------------------------

pound_detect_platform(POUND_PLATFORM POUND_TARGET_IS_ANDROID POUND_TARGET_IS_POSIX)
pound_detect_architecture(POUND_TARGET_ARCHITECTURE)
pound_set_shared_library_defaults()
pound_resolve_sanitizers()

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------

option(POUND_BUILD_TESTS
        "Build the Pound unit test suite and register it with CTest"
        ON
        )

option(POUND_ENABLE_INSTALL_RULES
        "Emit install() rules for the emulator and its runtime dependencies"
        ON
        )

option(POUND_LINK_GUI_STATICALLY
        "Link PoundGui into the emulator image instead of loading it at runtime"
        ${POUND_TARGET_IS_ANDROID}
        )

if (POUND_LINK_GUI_STATICALLY AND POUND_PLATFORM_SUPPORTS_HOT_RELOAD)
    message(STATUS
            "[Pound] GUI will be linked statically; the hot-reload loader in src/main.c "
            "is bypassed at runtime.")
endif ()

# -----------------------------------------------------------------------------
# Output directories
# -----------------------------------------------------------------------------
#
# Android is special: every native artefact has to land under jniLibs/<abi> so
# the Gradle packaging step can pick it up unchanged.

if (POUND_TARGET_IS_ANDROID)
    set(POUND_BINARIES_DIRECTORY "${CMAKE_BINARY_DIR}/jniLibs/${POUND_ANDROID_ABI_DIR}")
else ()
    set(POUND_BINARIES_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${POUND_PLATFORM}")
endif ()

set(POUND_LIBRARIES_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${POUND_PLATFORM}")

# Shared libraries land next to the executable on every platform, which is also
# what the loader's rpath/loader-path lookup expects.
set(POUND_RUNTIME_LIBRARY_DIR "${POUND_BINARIES_DIRECTORY}")

foreach (CONFIG IN ITEMS "" _DEBUG _RELEASE _MINSIZEREL _RELWITHDEBINFO)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${CONFIG} "${POUND_BINARIES_DIRECTORY}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${CONFIG} "${POUND_BINARIES_DIRECTORY}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${CONFIG} "${POUND_LIBRARIES_DIRECTORY}")
endforeach ()

# -----------------------------------------------------------------------------
# Install layout
# -----------------------------------------------------------------------------
#
# Defined before include(Ballistic), which installs the prebuilt JIT artefacts
# relative to these directories.

include(GNUInstallDirs)

if (POUND_TARGET_IS_ANDROID)
    set(POUND_INSTALL_BIN_DIR "lib/${POUND_ANDROID_ABI_DIR}")
    set(POUND_INSTALL_LIB_DIR "lib/${POUND_ANDROID_ABI_DIR}")
elseif (WIN32)
    set(POUND_INSTALL_BIN_DIR "bin")
    set(POUND_INSTALL_LIB_DIR "lib")
else ()
    set(POUND_INSTALL_BIN_DIR "${CMAKE_INSTALL_BINDIR}")
    set(POUND_INSTALL_LIB_DIR "${CMAKE_INSTALL_LIBDIR}")
endif ()

# -----------------------------------------------------------------------------
# Configuration summary
# -----------------------------------------------------------------------------

message(STATUS "==================================================================")
message(STATUS "[Pound] Configuring Pound Emulator v${PROJECT_VERSION}")
message(STATUS "[Pound] Build Type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "[Pound] Target OS:   ${CMAKE_SYSTEM_NAME} (${POUND_PLATFORM})")
message(STATUS "[Pound] Target CPU:  ${CMAKE_SYSTEM_PROCESSOR} (${POUND_TARGET_ARCHITECTURE})")
message(STATUS "[Pound] Compiler:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "[Pound] Binaries:    ${POUND_BINARIES_DIRECTORY}")
message(STATUS "[Pound] Libraries:   ${POUND_LIBRARIES_DIRECTORY}")
message(STATUS "[Pound] Tests:       ${POUND_BUILD_TESTS}")
message(STATUS "[Pound] Sanitizers:  ${POUND_SANITIZERS}")
message(STATUS "==================================================================")

if (POUND_TARGET_IS_ANDROID)
    message(STATUS "[Pound] Android ABI: ${POUND_ANDROID_ABI_DIR}")
endif ()

# -----------------------------------------------------------------------------
# Link time optimization
# -----------------------------------------------------------------------------

if (POUND_ENABLE_LINK_TIME_OPTIMIZATION)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)

    if (ipo_supported)
        message(STATUS "[Pound] Link Time Optimization is ENABLED and supported.")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else ()
        message(WARNING
                "[Pound] Link Time Optimization was requested but is NOT supported by your compiler, disabling LTO. "
                "Reason: ${ipo_error}")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
    endif ()
else ()
    message(STATUS "[Pound] Link Time Optimization is DISABLED via CMake options.")
endif ()

# -----------------------------------------------------------------------------
# Compile Pound
# -----------------------------------------------------------------------------

include(Ballistic)

message(STATUS "[Mimalloc] Configuring Mimalloc...")

if (POUND_TARGET_IS_ANDROID)
    # A shared mimalloc would have to be staged and loaded before mimalloc's
    # own malloc override takes effect, which the Android loader cannot guarantee
    # for the libc++ initialisation that runs first. A static archive avoids the
    # ordering problem entirely.
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(POUND_MIMALLOC_TARGET "mimalloc-static")
else ()
    set(POUND_MIMALLOC_TARGET "mimalloc")
endif ()

set(MI_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(MI_SECURE        ON  CACHE BOOL "" FORCE)
set(MI_OVERRIDE      ON  CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)

add_subdirectory(extern/mimalloc/mimalloc)

message(STATUS "[ImGUI] Configuring ImGUI...")
add_subdirectory(extern/cimgui)

if (POUND_LINK_GUI_STATICALLY)
    set(POUND_IMGUI_LIBRARY_TYPE STATIC)
else ()
    set(POUND_IMGUI_LIBRARY_TYPE SHARED)
endif ()

add_library(imgui ${POUND_IMGUI_LIBRARY_TYPE}
        ${cimgui_SOURCE_DIR}/cimgui.cpp
        ${cimgui_SOURCE_DIR}/imgui/imgui.cpp
        ${cimgui_SOURCE_DIR}/imgui/imgui_draw.cpp
        ${cimgui_SOURCE_DIR}/imgui/imgui_demo.cpp
        ${cimgui_SOURCE_DIR}/imgui/imgui_tables.cpp
        ${cimgui_SOURCE_DIR}/imgui/imgui_widgets.cpp
        ${cimgui_SOURCE_DIR}/imgui/backends/imgui_impl_opengl3.cpp
        ${cimgui_SOURCE_DIR}/imgui/backends/imgui_impl_sdl3.cpp
)
target_include_directories(imgui PUBLIC ${cimgui_SOURCE_DIR} ${cimgui_SOURCE_DIR}/imgui)
target_compile_definitions(imgui PUBLIC "-DIMGUI_IMPL_API=extern \"C\" ")
target_compile_definitions(imgui PUBLIC -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=1)
target_link_libraries(imgui PRIVATE SDL3::SDL3)
set_target_properties(imgui PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>"
)
set_target_properties(imgui PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# clang-cl doesn't receive /EHsc by default; mimalloc's alloc.c is compiled
# as C++ and requires exceptions for its new/delete overrides.
foreach (mi_target mimalloc mimalloc-static mimalloc-obj)
    if (TARGET ${mi_target})
        target_compile_options(${mi_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
    endif ()
endforeach ()

message(STATUS "[SDL] Configuring SDL library...")

if (POUND_TARGET_IS_ANDROID)
    # SDL is linked into libmain.so. Two copies of SDL state in one process
    # crash ImGui_ImplSDL3_Init, exactly as it does on Windows with the debug CRT.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
else ()
    # Shared SDL so Pound and imgui use one SDL instance.
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif ()

set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

# Bare minimum required for rendering.
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_RENDER ON CACHE BOOL "" FORCE)
set(SDL_EVENTS ON CACHE BOOL "" FORCE)
set(SDL_THREADS ON CACHE BOOL "" FORCE)
set(SDL_TIMERS ON CACHE BOOL "" FORCE)

# Disable everything nonessential.
#
# NOTE: SDL_AUDIO and SDL_JOYSTICK are currently off, which means the emulator
# has no audio output and no gamepad input on any platform. See docs/BUILDING.md
# for how to re-enable them; they are left off here to preserve the existing
# build configuration.
foreach (sdl_option
        SDL_AUDIO
        SDL_JOYSTICK
        SDL_HAPTIC
        SDL_SENSOR
        SDL_POWER
        SDL_HIDAPI
        SDL_CAMERA
        SDL_GPU
        SDL_STORAGE
        SDL_DIALOG
        SDL_FILE
        SDL_FILESYSTEM
        SDL_LOCALE
        SDL_MISC
        SDL_ATOMIC
        SDL_CPUINFO
        SDL_X11_XSCRNSAVER
        SDL_X11_XTEST
        SDL_WAYLAND_LIBDECOR
)
    set(${sdl_option} OFF CACHE BOOL "" FORCE)
endforeach ()

add_subdirectory(extern/sdl/sdl)

# Suppress clang warnings.
if (TARGET SDL3-shared)
    set_target_properties(SDL3-shared PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>"
    )
endif ()

message(STATUS "[PoundCore] Configuring Pound Core Library...")
add_library(PoundCore OBJECT
        src/core/memory/memory.c
        src/core/memory/guest_memory.c
        src/core/guest_state.c
        src/core/log.c
        src/core/safe_math.c
)
target_include_directories(PoundCore PUBLIC src/core)
target_link_libraries(PoundCore PRIVATE ${POUND_MIMALLOC_TARGET})
set_target_properties(PoundCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_compiler_flags(PoundCore)
add_linker_flags(PoundCore)
add_sanitizers(PoundCore)

message(STATUS "[PoundGUI] Configuring Pound GUI Library...")

if (POUND_LINK_GUI_STATICALLY)
    add_library(PoundGui STATIC
            src/gui/gui.c
            src/gui/gui_hot_reload.c
            src/gui/gui_layout.c
            src/gui/debug/debug_memory_data.c
            src/gui/debug/debug_memory_render.c
    )
    # A static archive must keep its entry points visible or the executable's
    # link would drop them as unreferenced before src/main.c can bind to them.
    set_target_properties(PoundGui PROPERTIES C_VISIBILITY_PRESET default)
else ()
    add_library(PoundGui SHARED
            src/gui/gui.c
            src/gui/gui_hot_reload.c
            src/gui/gui_layout.c
            src/gui/debug/debug_memory_data.c
            src/gui/debug/debug_memory_render.c
    )
    set_target_properties(PoundGui PROPERTIES C_VISIBILITY_PRESET hidden)
    set_target_properties(PoundGui PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

    if (WIN32)
        # MSVC/clang-cl do not prefix DLLs with "lib"; the loader looks for
        # libPoundGui.dll.
        set_target_properties(PoundGui PROPERTIES PREFIX "lib")
    endif ()
endif ()

target_include_directories(PoundGui PUBLIC src/gui src/core)
# Windows loads PoundGui before the EXE finishes its own imports. Listing
# mimalloc ahead of imgui keeps mimalloc-redirect ahead of ucrt in PoundGui's
# import table.
target_link_libraries(PoundGui PRIVATE ${POUND_MIMALLOC_TARGET} imgui)

if (MSVC)
    target_link_options(PoundGui PRIVATE "/INCLUDE:mi_version")
endif ()

add_compiler_flags(PoundGui)
add_linker_flags(PoundGui)
add_sanitizers(PoundGui)

message(STATUS "[Pound] Configuring Pound Emulator...")

if (POUND_TARGET_IS_ANDROID)
    # SDL's Android entry point is not a process entry point. SDLActivity
    # dlopen()s libmain.so and calls the exported SDL_main() through JNI, and
    # src/main.c obtains that symbol by including <SDL3/SDL_main.h>.
    add_library(${PROJECT_NAME} SHARED src/main.c)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "main")
else ()
    add_executable(${PROJECT_NAME} src/main.c)
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE src/)

target_link_libraries(${PROJECT_NAME} PRIVATE
        ${POUND_MIMALLOC_TARGET}
        PoundCore
        PoundGui
        Ballistic::Engine
        SDL3::SDL3
        imgui
)

if (MSVC)
    # Keep mimalloc.dll mapped so redirect can override malloc for the whole process.
    target_link_options(${PROJECT_NAME} PRIVATE "/INCLUDE:mi_version")
endif ()

target_compile_definitions(${PROJECT_NAME} PUBLIC -DCIMGUI_USE_OPENGL3 -DCIMGUI_USE_SDL3)
add_compiler_flags(${PROJECT_NAME})
add_linker_flags(${PROJECT_NAME})
add_sanitizers(${PROJECT_NAME})

# -----------------------------------------------------------------------------
# Android JNI packaging
# -----------------------------------------------------------------------------

if (POUND_TARGET_IS_ANDROID)
    # First pass at configure time: creates jniLibs/<abi> and copies anything
    # produced by the dependency build before Pound itself is compiled.
    pound_collect_jni_artifacts("${CMAKE_BINARY_DIR}")

    # Second pass after the link, when libmain.so actually exists.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
            -DPOUND_STAGE_DIR=${CMAKE_BINARY_DIR}
            -DPOUND_STAGE_SOURCE=$<TARGET_FILE:${PROJECT_NAME}>
            -P ${CMAKE_SOURCE_DIR}/cmake/StageJniArtifacts.cmake
            COMMENT "[Pound] Staging JNI artifacts for ${POUND_ANDROID_ABI_DIR}"
            VERBATIM
    )
endif ()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

if (POUND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
else ()
    message(STATUS "[Pound] Tests are DISABLED via CMake options.")
endif ()

# -----------------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------------

if (POUND_ENABLE_INSTALL_RULES)
    if (POUND_TARGET_IS_ANDROID)
        # libmain.so plus, once ARM64 Ballistic artefacts exist, libluajit.so,
        # which Ballistic.cmake installs into the same directory.
        install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION "${POUND_INSTALL_LIB_DIR}")
    else ()
        install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${POUND_INSTALL_BIN_DIR}")

        if (NOT POUND_LINK_GUI_STATICALLY)
            install(TARGETS PoundGui imgui
                    RUNTIME DESTINATION "${POUND_INSTALL_BIN_DIR}"
                    LIBRARY DESTINATION "${POUND_INSTALL_LIB_DIR}"
            )
        endif ()

        if (POUND_TARGET_IS_POSIX)
            install(TARGETS ${POUND_MIMALLOC_TARGET}
                    LIBRARY DESTINATION "${POUND_INSTALL_LIB_DIR}"
            )
        endif ()
    endif ()

    install(FILES README.md
            DESTINATION "${CMAKE_INSTALL_DOCDIR}"
            OPTIONAL
    )
else ()
    message(STATUS "[Pound] Install rules are DISABLED via CMake options.")
endif ()

message(STATUS "==================================================================")
message(STATUS "[Pound] Cmake configuration completed successfully.")
message(STATUS "==================================================================")