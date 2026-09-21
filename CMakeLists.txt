cmake_minimum_required(VERSION 4.3)
# The one place the version lives (semantic versioning, see README). Releases are tagged vX.Y.Z.
project(clipboard_history VERSION 1.1.1)

set(CMAKE_CXX_STANDARD 20)

# app.rc holds the version resource (and, for MinGW, the manifest).
add_executable(clipboard_history WIN32 main.cpp app.rc)

# version.h hands the version to main.cpp and app.rc.
configure_file(version.h.in version.h @ONLY)
target_include_directories(clipboard_history PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# MSVC embeds a .manifest source itself; MinGW needs it wrapped in the resource script.
if (MSVC)
    target_sources(clipboard_history PRIVATE app.manifest)
    target_compile_definitions(clipboard_history PRIVATE APP_MANIFEST_BY_LINKER)
else ()

    # Link the runtime statically so the exe runs on its own. With the posix thread model libstdc++
    # pulls in winpthread, and CLion's bundled MinGW ships a static winpthread that was built
    # against UCRT while the toolchain defaults to msvcrt. That leaves two symbols unresolved:
    #   __intrinsic_setjmpex  UCRT's name for msvcrt's _setjmpex, so alias it
    #   __ms_vsnprintf        lives in libmingwex, which is scanned before libmsvcrt asks for it,
    #                         so pull it in up front
    # Only apply the fixups where a plain -static link fails.
    include(CheckCXXSourceCompiles)
    set(static_probe "#include <string>
        int main(int argc, char **) { static std::string s(argc, 'x'); return (int) s.size(); }")
    set(static_fixups -Wl,--defsym,__intrinsic_setjmpex=_setjmpex -Wl,-u,__ms_vsnprintf)

    set(CMAKE_REQUIRED_LINK_OPTIONS -static)
    check_cxx_source_compiles("${static_probe}" STATIC_LINK_WORKS)
    if (NOT STATIC_LINK_WORKS)
        set(CMAKE_REQUIRED_LINK_OPTIONS -static ${static_fixups})
        check_cxx_source_compiles("${static_probe}" STATIC_LINK_WORKS_WITH_FIXUPS)
    endif ()
    unset(CMAKE_REQUIRED_LINK_OPTIONS)

    if (STATIC_LINK_WORKS)
        target_link_options(clipboard_history PRIVATE -static)
    elseif (STATIC_LINK_WORKS_WITH_FIXUPS)
        target_link_options(clipboard_history PRIVATE -static ${static_fixups})
    else ()
        message(WARNING "Static link failed; clipboard_history.exe will need libwinpthread-1.dll")
        target_link_options(clipboard_history PRIVATE -static-libgcc -static-libstdc++)
    endif ()
endif ()

target_compile_definitions(clipboard_history PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(clipboard_history PRIVATE
        user32 gdi32 shell32 advapi32 crypt32 dwmapi uxtheme shcore)
