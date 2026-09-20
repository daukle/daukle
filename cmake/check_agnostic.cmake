# plugins.c/.h stay out: their own manifest-table key text collides with a forbidden literal below
set(CORE_FILES
    resolve.c resolve.h manifest.c manifest.h registry.c registry.h
    types.h sync.c sync.h main.c cli.c cli.h config.c config.h)

# FR_CONFIG_ is exempt: the TOML/Lua config bootstrap floor is required, not a plugin
set(FORBIDDEN "\"gradle\"" "\"path\"" "\"npm\"" "daukle\\.source/[a-z]"
              "daukle\\.language/[a-z]" "daukle\\.config/[a-z]"
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
