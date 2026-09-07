cmake_minimum_required(VERSION 3.15)
project(PatchKnob_root VERSION 0.8.7 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# PatchKnob 0.8.7 - SDL2 multi-platform build.
#
# Build with the MSYS2 MINGW64 toolchain:
#
#   export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build -j
#
# On Windows the SDL target stages its MSYS runtime DLL closure next to PatchKnob.exe.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    # DSP builds must be optimized by default. In particular, Fundamental's
    # VCO/VCF use Rack float_4 intrinsics whose debug expansion is extremely
    # expensive at audio rate. Developers can still request Debug explicitly.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(sdlui)

if(WIN32)
    add_custom_target(nsis_installer
        COMMAND powershell -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_nsis.ps1"
                -ProjectRoot "${CMAKE_CURRENT_SOURCE_DIR}"
                -BuildDir "${CMAKE_BINARY_DIR}"
                -SkipBuild
        DEPENDS PatchKnob
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# INSTALL / PACKAGING (Linux)
#
# The binary goes to /usr/lib/patchknob rather than /usr/bin, with a symlink
# from /usr/bin/patchknob, so the Csound manual can sit BESIDE the executable.
# The app resolves the manual relative to its own directory first, and
# SDL_GetBasePath resolves through the symlink via /proc/self/exe -- so the
# packaged layout finds it without a compiled-in absolute path, which would
# otherwise point at whatever machine built the package.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE AND NOT ANDROID)
    include(GNUInstallDirs)
    set(PK_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}/patchknob")

    install(TARGETS PatchKnob RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/patchknob")

    # /usr/bin/patchknob -> ../lib/patchknob/PatchKnob
    #
    # DESTDIR must be honoured explicitly: CMake applies it to install(FILES)
    # and install(TARGETS) but NOT to paths a script builds itself, so using
    # CMAKE_INSTALL_PREFIX alone would write the symlink to the real /usr/bin
    # of the machine doing the packaging instead of into the staging tree.
    install(CODE "
        set(_pk_destdir \"\$ENV{DESTDIR}\")
        set(_pk_bin \"\${_pk_destdir}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
        file(MAKE_DIRECTORY \"\${_pk_bin}\")
        execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
            \"../${CMAKE_INSTALL_LIBDIR}/patchknob/PatchKnob\"
            \"\${_pk_bin}/patchknob\")
    ")

    # The in-app Csound manual, next to the executable.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/csound-manual/index.html")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/vendor/csound-manual"
                DESTINATION "${CMAKE_INSTALL_LIBDIR}/patchknob")
    endif()

    # Metronome click the app embeds a copy of but also loads by name.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Click.wav")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Click.wav"
                DESTINATION "${CMAKE_INSTALL_LIBDIR}/patchknob")
    endif()

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/patchknob.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/patchknob.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/patchknob")
endif()
