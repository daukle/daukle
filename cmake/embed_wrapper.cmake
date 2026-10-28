# Compiles the two wrapper scripts into a C source, byte for byte, so `daukle init` writes them
# with nothing beside the binary and they always match the daukle that wrote them. Read as hex
# rather than as text because file(READ) would otherwise fold the bytes through CMake's list and
# escape handling.

if(NOT DEFINED POSIX_SCRIPT OR NOT DEFINED POWERSHELL_SCRIPT OR NOT DEFINED OUT)
    message(FATAL_ERROR
            "embed_wrapper: usage is -DPOSIX_SCRIPT=<wrapper/daukle> "
            "-DPOWERSHELL_SCRIPT=<wrapper/daukle.ps1> -DOUT=<generated .c> -P cmake/embed_wrapper.cmake")
endif()

function(render_array symbol source output_variable)
    file(READ "${source}" hex HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR byte_count "${hex_length} / 2")
    if(byte_count EQUAL 0)
        message(FATAL_ERROR "embed_wrapper: ${source} is empty")
    endif()
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    # CMake's regex has no {n}, so the sixteen-byte row is spelled out.
    string(REPEAT "0x[0-9a-f][0-9a-f]," 16 row)
    string(REGEX REPLACE "(${row})" "\\1\n    " bytes "${bytes}")
    set(${output_variable}
        "const unsigned char ${symbol}[] = {\n    ${bytes}\n};\nconst size_t ${symbol}_length = ${byte_count};\n"
        PARENT_SCOPE)
endfunction()

render_array(fr_wrapper_posix_script "${POSIX_SCRIPT}" posix)
render_array(fr_wrapper_powershell_script "${POWERSHELL_SCRIPT}" powershell)

set(rendered "#include \"cli/wrapper_scripts.h\"\n\n${posix}\n${powershell}")

# Written only when it changed, so a reconfigure does not rebuild everything that links it, and
# touched otherwise, or an input newer than it reruns this on every build.
if(EXISTS "${OUT}")
    file(READ "${OUT}" previous)
    if(previous STREQUAL rendered)
        file(TOUCH_NOCREATE "${OUT}")
        return()
    endif()
endif()
file(WRITE "${OUT}" "${rendered}")
