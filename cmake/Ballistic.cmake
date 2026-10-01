include_guard(GLOBAL)

# -----------------------------------------------------------------------------
# Ballistic JIT engine
# -----------------------------------------------------------------------------
#
# Ballistic is consumed as a prebuilt static library plus a LuaJIT shared
# library. Prebuilt artefacts are committed per platform under
# `extern/ballistic/<platform>/`, and the layout differs enough between MSVC
# and ELF that each one is described explicitly rather than pattern matched.
#
# ARM note: no ARM64 artefacts are committed yet. `POUND_ENABLE_BALLISTIC`
# therefore defaults to ON only where the artefacts actually exist, which lets
# the ARM/Android and aarch64 CI lanes build and test the rest of Pound today
# and pick the JIT up automatically once `extern/ballistic/android/arm64-v8a`
# and `extern/ballistic/linux-arm64` are populated.
#
# Expected layouts:
#   windows/     lib/Ballistic.lib        lib/lua51.lib       bin/lua51.dll
#   linux/       lib/libBallistic.a                        bin/libluajit.so
#   android/     lib/<abi>/libBallistic.a                  lib/<abi>/libluajit.so

function(ballistic_verify_directory path description)
    string(STRIP "${path}" path)

    if (NOT IS_DIRECTORY "${path}")
        message(FATAL_ERROR "[Ballistic] Missing ${description}: ${path}")
    endif ()
endfunction()

function(ballistic_verify_file path description)
    string(STRIP "${path}" path)

    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "[Ballistic] Missing ${description}: ${path}")
    endif ()

    if (IS_DIRECTORY "${path}")
        message(FATAL_ERROR "[Ballistic] ${description} is a directory, expected a file: ${path}")
    endif ()
endfunction()

function(ballistic_verify_imported_path target property)
    get_target_property(path ${target} ${property})

    if (NOT path)
        return()
    endif ()

    if (path STREQUAL "path-NOTFOUND")
        return()
    endif ()

    string(STRIP "${path}" path)

    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "[Ballistic] ${target} ${property} does not exist: ${path}")
    endif ()

    if (IS_DIRECTORY "${path}")
        message(FATAL_ERROR "[Ballistic] ${target} ${property} is a directory: ${path}")
    endif ()

    set_target_properties(${target} PROPERTIES ${property} "${path}")
endfunction()

set(BALLISTIC_ROOT "${PROJECT_SOURCE_DIR}/extern/ballistic")

if (NOT IS_DIRECTORY "${BALLISTIC_ROOT}/include")
    message(FATAL_ERROR
            "[Ballistic] Missing Ballistic headers: ${BALLISTIC_ROOT}/include. "
            "extern/ballistic is vendored in-tree, so this means the checkout is "
            "incomplete; re-clone the repository.")
endif ()

ballistic_verify_file("${BALLISTIC_ROOT}/include/bal_engine.h" "bal_engine.h header")

# -----------------------------------------------------------------------------
# Option
# -----------------------------------------------------------------------------

option(POUND_ENABLE_BALLISTIC
        "Link the Ballistic ARM64 JIT recompiler when prebuilt artefacts exist"
        ON
        )

# Resolves the artefact paths for the current target, or leaves them empty when
# this platform has no prebuilt build.
function(ballistic_resolve_artefacts out_engine out_luajit_implib out_luajit_runtime)
    set(engine "")
    set(luajit_implib "")
    set(luajit_runtime "")

    if (WIN32)
        set(engine        "${BALLISTIC_ROOT}/windows/lib/Ballistic.lib")
        set(luajit_implib "${BALLISTIC_ROOT}/windows/lib/lua51.lib")
        set(luajit_runtime "${BALLISTIC_ROOT}/windows/bin/lua51.dll")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(abi "${POUND_ANDROID_ABI_DIR}")
        set(engine         "${BALLISTIC_ROOT}/android/lib/${abi}/libBallistic.a")
        set(luajit_runtime "${BALLISTIC_ROOT}/android/lib/${abi}/libluajit.so")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(engine         "${BALLISTIC_ROOT}/mac/lib/libBallistic.a")
        set(luajit_runtime "${BALLISTIC_ROOT}/mac/lib/libluajit.dylib")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(engine         "${BALLISTIC_ROOT}/linux/lib/libBallistic.a")
        set(luajit_runtime "${BALLISTIC_ROOT}/linux/bin/libluajit.so")
    endif ()

    set(${out_engine}         "${engine}" PARENT_SCOPE)
    set(${out_luajit_implib}  "${luajit_implib}" PARENT_SCOPE)
    set(${out_luajit_runtime} "${luajit_runtime}" PARENT_SCOPE)
endfunction()

ballistic_resolve_artefacts(BALLISTIC_ENGINE_LIBRARY
                            BALLISTIC_LUAJIT_IMPLIB
                            BALLISTIC_LUAJIT_RUNTIME
                            )

# Every artefact must be present together; a partial set is treated as absent
# so the build never half-links.
set(BALLISTIC_ARTEFACTS_PRESENT TRUE)

if (BALLISTIC_ENGINE_LIBRARY STREQUAL "" OR BALLISTIC_LUAJIT_RUNTIME STREQUAL "")
    set(BALLISTIC_ARTEFACTS_PRESENT FALSE)
endif ()

foreach (artefact IN ITEMS BALLISTIC_ENGINE_LIBRARY BALLISTIC_LUAJIT_RUNTIME BALLISTIC_LUAJIT_IMPLIB)
    if (${artefact} STREQUAL "")
        continue()
    endif ()

    if (NOT EXISTS "${${artefact}}")
        message(STATUS "[Ballistic] Not present: ${${artefact}}")
        set(BALLISTIC_ARTEFACTS_PRESENT FALSE)
    endif ()
endforeach ()

if (NOT BALLISTIC_ARTEFACTS_PRESENT)
    if (POUND_ENABLE_BALLISTIC)
        message(WARNING
                "[Ballistic] No prebuilt engine for '${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}'. "
                "Disabling the JIT; the emulator shell, GUI and test suite still build.")
    endif ()

    set(POUND_ENABLE_BALLISTIC OFF CACHE BOOL "" FORCE)
endif ()

if (POUND_ENABLE_BALLISTIC)
    message(STATUS "[Ballistic] Linking prebuilt engine for ${CMAKE_SYSTEM_NAME}.")

    ballistic_verify_file("${BALLISTIC_ENGINE_LIBRARY}" "Ballistic engine library")
    ballistic_verify_file("${BALLISTIC_LUAJIT_RUNTIME}" "LuaJIT shared library")

    if (WIN32)
        ballistic_verify_file("${BALLISTIC_LUAJIT_IMPLIB}" "LuaJIT import library")
    endif ()

    add_library(Ballistic::Engine STATIC IMPORTED GLOBAL)

    set_target_properties(Ballistic::Engine PROPERTIES
            IMPORTED_LOCATION "${BALLISTIC_ENGINE_LIBRARY}"
    )

    add_library(Ballistic::LuaJIT SHARED IMPORTED GLOBAL)

    if (WIN32)
        set_target_properties(Ballistic::LuaJIT PROPERTIES
                IMPORTED_IMPLIB "${BALLISTIC_LUAJIT_IMPLIB}"
                IMPORTED_LOCATION "${BALLISTIC_LUAJIT_RUNTIME}"
        )
    else ()
        set_target_properties(Ballistic::LuaJIT PROPERTIES
                IMPORTED_LOCATION "${BALLISTIC_LUAJIT_RUNTIME}"
                # The prebuilt libluajit has no SONAME; without this CMake
                # synthesises one and the loader then looks for a file that does
                # not exist next to the executable.
                IMPORTED_NO_SONAME TRUE
        )
    endif ()

    target_link_libraries(Ballistic::Engine INTERFACE Ballistic::LuaJIT)

    ballistic_verify_imported_path(Ballistic::Engine IMPORTED_LOCATION)
    ballistic_verify_imported_path(Ballistic::LuaJIT IMPORTED_LOCATION)

    if (WIN32)
        ballistic_verify_imported_path(Ballistic::LuaJIT IMPORTED_IMPLIB)
    endif ()
else ()
    # A no-op target keeps `if (TARGET Ballistic::Engine)` call sites in the
    # source working without preprocessor guards in every file.
    add_library(Ballistic::Engine INTERFACE IMPORTED GLOBAL)
    message(STATUS "[Ballistic] JIT disabled; linking an inert placeholder target.")
endif ()

# -----------------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------------

if (POUND_ENABLE_BALLISTIC AND POUND_ENABLE_INSTALL_RULES)
    if (ANDROID)
        # Flat layout: everything an APK packages has to sit directly under
        # lib/<abi>/, which is where the Gradle native-library directory is
        # mapped from.
        install(FILES "${BALLISTIC_ENGINE_LIBRARY}"
                DESTINATION "${POUND_INSTALL_LIB_DIR}"
                )
        install(FILES "${BALLISTIC_LUAJIT_RUNTIME}"
                DESTINATION "${POUND_INSTALL_LIB_DIR}"
                )
    elseif (WIN32)
        install(FILES "${BALLISTIC_ENGINE_LIBRARY}" "${BALLISTIC_LUAJIT_IMPLIB}"
                DESTINATION "${POUND_INSTALL_LIB_DIR}/ballistic"
                )
        install(FILES "${BALLISTIC_LUAJIT_RUNTIME}"
                DESTINATION "${POUND_INSTALL_BIN_DIR}"
                )
    else ()
        install(FILES "${BALLISTIC_ENGINE_LIBRARY}"
                DESTINATION "${POUND_INSTALL_LIB_DIR}/ballistic"
                )
        install(FILES "${BALLISTIC_LUAJIT_RUNTIME}"
                DESTINATION "${POUND_INSTALL_LIB_DIR}"
                )
    endif ()
endif ()
