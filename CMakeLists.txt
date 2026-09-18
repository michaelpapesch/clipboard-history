cmake_minimum_required(VERSION 4.3)
project(clipboard_history)

set(CMAKE_CXX_STANDARD 20)

add_executable(clipboard_history WIN32 main.cpp)

# MSVC embeds a .manifest source itself; MinGW needs it wrapped in a resource script.
if (MSVC)
    target_sources(clipboard_history PRIVATE app.manifest)
else ()
    target_sources(clipboard_history PRIVATE app.rc)
    target_link_options(clipboard_history PRIVATE -static-libgcc -static-libstdc++)

    # CLion's bundled MinGW can't link winpthread statically (its static lib targets UCRT, the
    # toolchain defaults to msvcrt), so ship the DLL next to the exe instead.
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_file(WINPTHREAD_DLL libwinpthread-1.dll HINTS "${compiler_dir}" NO_DEFAULT_PATH)
    if (WINPTHREAD_DLL)
        add_custom_command(TARGET clipboard_history POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WINPTHREAD_DLL}" "$<TARGET_FILE_DIR:clipboard_history>")
    endif ()
endif ()

target_compile_definitions(clipboard_history PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(clipboard_history PRIVATE
        user32 gdi32 shell32 advapi32 crypt32 dwmapi uxtheme shcore)
