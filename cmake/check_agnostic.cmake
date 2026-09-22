# plugins.c/.h stay out: their own manifest-table key text collides with a forbidden literal below
set(CORE_FILES
    resolve.c resolve.h manifest.c manifest.h registry.c registry.h
    types.h sync.c sync.h main.c cli.c cli.h config.c config.h)

# FR_CONFIG_ is exempt: the TOML/Lua config bootstrap floor is required, not a plugin
set(FORBIDDEN "\"gradle\"" "\"path\"" "\"npm\"" "daukle\\.source/[a-z]"
              "daukle\\.language/[a-z]" "daukle\\.config/[a-z]" "daukle\\.toolchain/[a-z]"
              "FR_SOURCE_[A-Z]" "FR_LANGUAGE_[A-Z]")

set(FINDINGS "")
foreach(name ${CORE_FILES})
    file(READ "${SOURCE_DIR}/src/${name}" content)
    foreach(pattern ${FORBIDDEN})
        if(content MATCHES "${pattern}")
            list(APPEND FINDINGS "${name} matches ${pattern}")
        endif()
    endforeach()
endforeach()

if(FINDINGS)
    foreach(finding ${FINDINGS})
        message(STATUS "agnostic-core: ${finding}")
    endforeach()
    message(FATAL_ERROR "core names a plugin")
endif()

# A fixture that needs a staged plugin carries its own copy, because a local
# plugin path resolves through a sandbox that refuses ".." and absolute paths.
# No test loads the staged file itself, so nothing but this loop would notice an
# edit to one copy, or to the staged original, that the others did not get.
file(GLOB STAGED_PLUGINS "${SOURCE_DIR}/plugins/*.lua")
file(GLOB_RECURSE FIXTURE_LUA "${SOURCE_DIR}/test/fixtures/*.lua")

set(DIVERGED "")
foreach(staged ${STAGED_PLUGINS})
    get_filename_component(staged_name "${staged}" NAME)
    file(READ "${staged}" staged_text)
    foreach(copy ${FIXTURE_LUA})
        get_filename_component(copy_name "${copy}" NAME)
        if(copy_name STREQUAL staged_name)
            file(READ "${copy}" copy_text)
            if(NOT copy_text STREQUAL staged_text)
                file(RELATIVE_PATH shown "${SOURCE_DIR}" "${copy}")
                list(APPEND DIVERGED "${shown} differs from plugins/${staged_name}")
            endif()
        endif()
    endforeach()
endforeach()

if(DIVERGED)
    foreach(finding ${DIVERGED})
        message(STATUS "staged-plugin-copies: ${finding}")
    endforeach()
    message(FATAL_ERROR "a fixture copy of a staged plugin is not byte identical")
endif()
