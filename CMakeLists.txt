# Captain America: Super Soldier - native PC build via ReXGlue.
#
# generated/ is produced from your own default.xex by `rexglue codegen` and is never committed.
# scripts/build.ps1 does the whole sequence.

cmake_minimum_required(VERSION 3.25)
project(cass LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Written by `rexglue codegen`.
include(generated/rexglue.cmake)

set(CASS_SOURCES
    src/main.cpp
    src/cass_pso_library.cpp      # persistent D3D12 pipeline library (from Judgment)
    src/cass_fsr.cpp              # SMAA 1x + FSR1 final pass (from Judgment)
    src/cass_pacing.cpp           # frame limiter + present dedup (from Judgment)
    src/cass_system_tuning.cpp    # power-throttling opt-out (from Judgment)
    src/cass_fps_overlay.cpp      # F9 FPS overlay (from Judgment)
    src/cass_render_size.cpp      # cass_native_720p switch for tools/patch_native_720p.py
    src/cass_paths.cpp            # cass_game_dir: game folder or .iso without --game_data_root
    src/cass_iso.cpp              # play straight from the .iso (DiscImageDevice on game:\ / d:\)
    src/cass_guest_trap.cpp       # CassGuestTrap / CassGuestUnimplemented for tools/patch_guest_traps.py
    src/cass_switch_trap.cpp      # jump-table defaults: follow CTR or REX_FATAL (tools/patch_switch_traps.py)
    src/cass_crash_guard.cpp      # crash guard: abort/terminate/unhandled-exception reports, minidumps, dialog
)

if(WIN32)
    add_executable(cass WIN32 ${CASS_SOURCES})
else()
    add_executable(cass ${CASS_SOURCES})
endif()

# Also stages the SDK runtime DLLs next to cass.exe.
rexglue_setup_target(cass GPU_PLUGINS xenos)

# MiniDumpWriteDump for src/cass_crash_guard.cpp.
target_link_libraries(cass PRIVATE dbghelp)

# Post-codegen patchers rewrite generated/default/*.cpp after `rexglue codegen` and before
# cass_recomp compiles them, chained by stamps because they edit the same files.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(CASS_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated/default)
set(_cass_prev_stamp ${CASS_GEN_DIR}/codegen.build.stamp)
macro(cass_patch name script description)
    set(_stamp ${CASS_GEN_DIR}/${name}.build.stamp)
    add_custom_command(OUTPUT ${_stamp}
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/${script} ${CASS_GEN_DIR}
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        DEPENDS ${_cass_prev_stamp} ${CMAKE_CURRENT_SOURCE_DIR}/tools/${script}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "${description}"
        VERBATIM)
    add_custom_target(cass_${name}_patch DEPENDS ${_stamp})
    if(TARGET cass_recomp)
        add_dependencies(cass_recomp cass_${name}_patch)
    endif()
    add_dependencies(cass cass_${name}_patch)
    set(_cass_prev_stamp ${_stamp})
endmacro()

# Render-size sites -> CassRenderWidth/Height (cass_native_720p, default off).
cass_patch(native720p patch_native_720p.py "Patching render-size sites (cass_native_720p)")

# REX_UNIMPLEMENTED sites (uncaught throw -> silent abort) -> CassGuestTrap (log once per site, continue)
# or CassGuestUnimplemented (log [FATAL], flush, abort). Fails the build on any unclassifiable site.
cass_patch(guesttraps patch_guest_traps.py "Patching REX_UNIMPLEMENTED sites (guest traps)")

# Jump-table defaults (`__builtin_trap(); // Switch case out of range`, a silent ud2) -> follow the guest
# CTR: a label of the same function, a registered function (tail call), else REX_FATAL naming the site.
cass_patch(switchtraps patch_switch_traps.py "Patching jump-table defaults (switch traps)")

# Default settings next to cass.exe, copied once so the player's edits are kept.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/cass.toml")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/config/cass.toml" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# ---------------------------------------------------------------------------------------------
# "Captain America Launcher.exe": the settings / launcher window next to cass.exe (Win32 + GDI+,
# adapted from the Gears of War: Judgment port's launcher/settings.cpp). Static C runtime and only
# Windows components, so it needs nothing installed. Its "Reset to defaults" uses
# config/cass.toml as it was at build time.
# ---------------------------------------------------------------------------------------------
if(WIN32)
    enable_language(RC)
    # clang++ is paired with rc.exe, and CMake's clang rule hands rc.exe clang's preprocessor and
    # depfile flags, which it rejects: compile the .rc with rc.exe alone (as the Gears 3 port does).
    set(CMAKE_DEPFILE_FLAGS_RC "")
    set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> /nologo <DEFINES> <INCLUDES> /fo <OBJECT> <SOURCE>")

    # cass.exe icon: our own shield (launcher/launcher.ico, original art) so the release carries
    # no game artwork. CASS_GAME_ICON=ON uses the game's dashboard tile from your default.xex
    # instead (personal builds only).
    option(CASS_GAME_ICON "Use the game's own title icon from default.xex for cass.exe" OFF)
    set(CASS_ICON ${CMAKE_CURRENT_BINARY_DIR}/cass_gen/cass.ico)
    if(CASS_GAME_ICON)
        add_custom_command(OUTPUT ${CASS_ICON}
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_game_icon.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex ${CASS_ICON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.ico
            DEPENDS tools/make_game_icon.py tools/dump_xex_image.py launcher/launcher.ico
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Building cass.exe icon from the game's title image"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${CASS_ICON}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.ico ${CASS_ICON}
            DEPENDS launcher/launcher.ico
            COMMENT "Using the launcher shield as cass.exe icon"
            VERBATIM)
    endif()
    target_sources(cass PRIVATE src/cass.rc ${CASS_ICON})
    set_source_files_properties(src/cass.rc PROPERTIES OBJECT_DEPENDS ${CASS_ICON})
    target_include_directories(cass PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/cass_gen)  # rc.exe finds cass.ico

    file(READ config/cass.toml CASS_DEFAULT_CONFIG)
    configure_file(launcher/default_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/launcher_gen/default_config.h @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/cass.toml)

    add_executable(cass_launcher WIN32 launcher/settings.cpp launcher/launcher.rc launcher/launcher.manifest)
    target_include_directories(cass_launcher PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/launcher_gen
        ${CMAKE_CURRENT_SOURCE_DIR}/launcher)  # rc.exe finds launcher.ico here
    set_source_files_properties(launcher/launcher.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.ico")
    target_compile_definitions(cass_launcher PRIVATE UNICODE _UNICODE)
    target_link_libraries(cass_launcher PRIVATE user32 gdi32 gdiplus dwmapi shell32 ole32 comctl32 dxgi)
    set_target_properties(cass_launcher PROPERTIES
        OUTPUT_NAME "Captain America Launcher"
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:cass>
        MSVC_RUNTIME_LIBRARY MultiThreaded)
    # Not a dependency of cass: an open launcher window would make every game build fail to
    # relink it. scripts/build.ps1 builds it as a second, non-fatal step.
endif()
