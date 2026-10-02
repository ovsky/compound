cmake_minimum_required(VERSION 3.25..3.31)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # Strict ISO C

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CmakePolicies)

# MSVC runtime selection must be fixed before any target is created, because
# CMP0091 is set to NEW in cmake/CmakePolicies.cmake: from that policy onwards
# CMake stops guessing and emits no /MD or /MT of its own, so a target created
# without this variable silently takes the compiler driver default instead.
#
# The dynamic runtime is required, not merely conventional. mimalloc's Windows
# redirection works by interposing on the CRT allocator inside ucrtbase.dll, so
# a statically linked CRT would leave nothing to interpose on and the override
# would silently never engage -- see requirement 1 in
# extern/mimalloc/mimalloc/bin/readme.md. SDL3, mimalloc and cimgui all inherit
# this from the enclosing scope, so the whole graph agrees on one runtime.
#
# The consequence is that a staged build needs the matching Visual C++
# redistributable (or app-local msvcp140.dll/vcruntime140.dll) present at run
# time; docs/BUILDING.md records that.
if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()

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

# `src/core/sync/mutex.c` maps onto pthread on POSIX. Modern glibc folds
# libpthread into libc, but older toolchains and musl-based Android NDKs do not,
# so the flag has to be requested rather than assumed. Failing here is better
# than an undefined-reference at link time on one CI lane only.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
message(STATUS "[Pound] Threads:      ${CMAKE_THREAD_LIBS_INIT} (${Threads_FOUND})")
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

# cimgui's own CMakeLists is deliberately not added as a subdirectory. It would
# define a `cimgui` target built from exactly the sources listed below, plus an
# unconditional `install(TARGETS cimgui ...)` rule that drops an unused
# cimgui.dll and its import library at the root of the install prefix. Pound
# links the `imgui` target defined just below and never the `cimgui` one, so
# pulling the subproject in would double the ImGui compile time and pollute the
# staged tree with a DLL nothing loads.
#
# The subproject also applies `IMGUI_USER_CONFIG=../cimconfig.h`, which is
# directory-scoped and therefore never reached the `imgui` target anyway.
# cimconfig.h only does `#undef NDEBUG`, which a Debug build already satisfies.
#
# Re-derive the two paths `add_subdirectory()` would have set.
set(cimgui_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/cimgui")
if (NOT EXISTS "${cimgui_SOURCE_DIR}/cimgui.cpp")
    message(FATAL_ERROR
            "[ImGUI] cimgui sources are missing from ${cimgui_SOURCE_DIR}. "
            "Initialise the 'extern' submodules before configuring.")
endif ()

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

# cimgui exports its core API through `CIMGUI_API`, which is
# `extern "C" __declspec(dllexport)` on Windows. The platform backends are a
# different macro: `IMGUI_IMPL_API`, which imgui.h only defaults to `IMGUI_API`
# -- and `IMGUI_API` defaults to *nothing*. Overriding `IMGUI_IMPL_API` with a
# bare `extern "C"` therefore stripped the dllexport from every
# `ImGui_ImplSDL3_*` / `ImGui_ImplOpenGL3_*` entry point, leaving them private to
# imgui.dll while the core stayed exported -- so the host linked against the
# core but not the backends it calls directly. The macro is `#ifndef`-guarded, so
# this definition has to carry the export attribute itself.
if (WIN32)
    set(POUND_IMGUI_IMPL_API "extern \"C\" __declspec(dllexport)")
else ()
    # ELF and Mach-O export by default visibility; nothing extra is needed.
    set(POUND_IMGUI_IMPL_API "extern \"C\"")
endif ()
target_compile_definitions(imgui PUBLIC "-DIMGUI_IMPL_API=${POUND_IMGUI_IMPL_API}")
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
        src/core/errors.c
        src/core/memory/memory.c
        src/core/memory/guest_memory.c
        src/core/memory/pool_allocator.c
        src/core/memory/slab_allocator.c
        src/core/jit/jit_cache.c
        src/core/sync/mutex.c
        src/core/guest_state.c
        src/core/log.c
        src/core/safe_math.c
)
target_include_directories(PoundCore PUBLIC src/core)
target_link_libraries(PoundCore PRIVATE ${POUND_MIMALLOC_TARGET} Threads::Threads)
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

# `PoundGui` calls into core -- `thread_logger`, `pound_log_message`,
# `memory_subsystem_get_memory_used_by_bucket`, `safe_math_error_string` -- so it
# has to resolve those symbols at link time.
#
# On desktop `PoundGui` is a SHARED image and linking `PoundCore` embeds the core
# objects into it, which is what makes the DLL self-contained: an unresolved
# symbol in a shared object is resolved against whatever happens to be loaded
# first, so relying on the host to export them would work on Windows by accident
# and fail on ELF. `gui_plugin_exports_get()` installs a default sink on the
# plugin's own thread-local logger so those records are emitted rather than
# silently dropped.
#
# On Android `PoundGui` is STATIC, and a static archive does not consume an
# OBJECT library's objects, so the plugin image stays core-free and `libmain.so`
# supplies the single copy -- which is what we want there.
#
# Windows loads PoundGui before the EXE finishes its own imports. Listing
# mimalloc ahead of imgui keeps mimalloc-redirect ahead of ucrt in PoundGui's
# import table.
target_link_libraries(PoundGui PRIVATE ${POUND_MIMALLOC_TARGET} imgui PoundCore)

if (MSVC)
    target_link_options(PoundGui PRIVATE "/INCLUDE:mi_version")
endif ()

add_compiler_flags(PoundGui)
add_linker_flags(PoundGui)
add_sanitizers(PoundGui)

message(STATUS "[Pound] Configuring Pound Emulator...")

# `jit_ballistic.c` is the only translation unit that includes the engine's headers,
# so it is added only where the engine is actually linked. The code cache it binds
# lives in PoundCore and is built everywhere, which is what lets the Android and
# aarch64 lanes test the W^X allocator even with no ARM64 prebuild present.
set(POUND_EMULATOR_SOURCES src/main.c)

if (POUND_ENABLE_BALLISTIC)
    list(APPEND POUND_EMULATOR_SOURCES src/core/jit/jit_ballistic.c)
endif ()

if (POUND_TARGET_IS_ANDROID)
    # SDL's Android entry point is not a process entry point. SDLActivity
    # dlopen()s libmain.so and calls the exported SDL_main() through JNI, and
    # src/main.c obtains that symbol by including <SDL3/SDL_main.h>.
    add_library(${PROJECT_NAME} SHARED ${POUND_EMULATOR_SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "main")
else ()
    add_executable(${PROJECT_NAME} ${POUND_EMULATOR_SOURCES})
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

        # SDL's own install rules are switched off above (SDL_INSTALL=OFF) so a
        # Pound release ships neither SDL's headers, its pkg-config file, nor its
        # CMake package config -- none of which the emulator needs at run time.
        # The consequence is that the SDL runtime is no longer staged either,
        # and the loader cannot find SDL3.dll next to Pound.exe, so the
        # installed executable fails to start. Stage it explicitly.
        #
        # Android is excluded because it links SDL statically (see above), and
        # `install(TARGETS ...)` on an archive-only target would install the
        # import library the APK has no loader for.
        if (TARGET SDL3-shared)
            install(TARGETS SDL3-shared
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