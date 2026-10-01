# The files that decide where a plugin comes from carry the "path" manifest-table key text, which
# collides with a forbidden literal below, so they stay out of this list. The forge-host rule
# further down reads every file in src/, so they remain covered against naming a host.
set(CORE_FILES
    plugin/resolve.c plugin/resolve.h config/manifest.c config/manifest.h plugin/registry.c plugin/registry.h
    util/types.h project/sync.c project/sync.h cli/main.c cli/cli.c cli/cli.h config/config.c config/config.h
    project/derived.c project/derived.h project/generate.c project/generate.h project/tasks.c project/tasks.h
    plugin/plugin_fetch.c plugin/plugin_fetch.h archive/tar.c archive/tar.h plugin/plugin_modules.c plugin/plugin_modules.h
    plugin/plugin_deps.c plugin/plugin_deps.h archive/archive.c archive/archive.h archive/unpack.c archive/unpack.h
    provision/provision.c provision/provision.h exec/toolreport.c exec/toolreport.h)

# FR_CONFIG_ is exempt: the TOML/Lua config bootstrap floor is required, not a plugin
set(FORBIDDEN "\"gradle\"" "\"path\"" "\"npm\"" "daukle\\.source/[a-z]"
              "daukle\\.language/[a-z]" "daukle\\.config/[a-z]" "daukle\\.toolchain/[a-z]"
              "daukle\\.task/[a-z]" "FR_SOURCE_[A-Z]" "FR_LANGUAGE_[A-Z]")

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

# Every file in src/, not just CORE_FILES: the files excluded above carry the
# "path" key precisely because they decide where a plugin comes from, so the
# list that catches a hardcoded host cannot be the list that excludes them.
file(GLOB_RECURSE ALL_SOURCES "${SOURCE_DIR}/src/*.c" "${SOURCE_DIR}/src/*.h")
# A glob that matches nothing reports no findings, which reads exactly like a
# clean run. Moving src/ into subdirectories turned this set empty once and the
# check still passed, so the count is asserted rather than trusted.
list(LENGTH ALL_SOURCES ALL_SOURCES_COUNT)
if(ALL_SOURCES_COUNT LESS 40)
    message(FATAL_ERROR "agnostic-core: found ${ALL_SOURCES_COUNT} sources under src/, "
                        "which is too few to be the whole program; the glob has gone stale")
endif()
set(FORBIDDEN_HOSTS "api\\.github\\.com" "github\\.com" "gitlab" "bitbucket")

set(HOST_FINDINGS "")
foreach(source ${ALL_SOURCES})
    file(READ "${source}" content)
    get_filename_component(source_name "${source}" NAME)
    foreach(pattern ${FORBIDDEN_HOSTS})
        if(content MATCHES "${pattern}")
            list(APPEND HOST_FINDINGS "${source_name} matches ${pattern}")
        endif()
    endforeach()
endforeach()

if(HOST_FINDINGS)
    foreach(finding ${HOST_FINDINGS})
        message(STATUS "agnostic-core: ${finding}")
    endforeach()
    message(FATAL_ERROR "core names a forge")
endif()

# A fixture that needs a staged plugin carries its own copy, because a local
# plugin path resolves through a sandbox that refuses ".." and absolute paths.
# No test loads the staged file itself, so nothing but this loop would notice an
# edit to one copy, or to the staged original, that the others did not get.
file(GLOB STAGED_PLUGINS "${SOURCE_DIR}/plugins/*.lua")
file(GLOB_RECURSE FIXTURE_LUA "${SOURCE_DIR}/test/fixtures/*.lua")

# Either glob going empty reads exactly like a clean run: deleting plugins/
# compares nothing and passes, measured. The count is asserted rather than
# trusted, same as the source glob above, and a staged plugin held identical to
# no copy at all is reported rather than skipped.
list(LENGTH STAGED_PLUGINS STAGED_PLUGINS_COUNT)
if(STAGED_PLUGINS_COUNT LESS 5)
    message(FATAL_ERROR "staged-plugin-copies: found ${STAGED_PLUGINS_COUNT} plugins under "
                        "plugins/, which is too few to be the staged set; the glob has gone stale")
endif()

set(DIVERGED "")
set(UNCOMPARED "")
foreach(staged ${STAGED_PLUGINS})
    get_filename_component(staged_name "${staged}" NAME)
    file(READ "${staged}" staged_text)
    set(copies_compared 0)
    foreach(copy ${FIXTURE_LUA})
        get_filename_component(copy_name "${copy}" NAME)
        if(copy_name STREQUAL staged_name)
            math(EXPR copies_compared "${copies_compared} + 1")
            file(READ "${copy}" copy_text)
            if(NOT copy_text STREQUAL staged_text)
                file(RELATIVE_PATH shown "${SOURCE_DIR}" "${copy}")
                list(APPEND DIVERGED "${shown} differs from plugins/${staged_name}")
            endif()
        endif()
    endforeach()
    if(copies_compared EQUAL 0)
        list(APPEND UNCOMPARED "plugins/${staged_name} is held identical to no fixture copy")
    endif()
endforeach()

if(UNCOMPARED)
    foreach(finding ${UNCOMPARED})
        message(STATUS "staged-plugin-copies: ${finding}")
    endforeach()
    message(FATAL_ERROR "a staged plugin is compared against nothing")
endif()

if(DIVERGED)
    foreach(finding ${DIVERGED})
        message(STATUS "staged-plugin-copies: ${finding}")
    endforeach()
    message(FATAL_ERROR "a fixture copy of a staged plugin is not byte identical")
endif()
