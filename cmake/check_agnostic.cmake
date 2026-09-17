set(CORE_FILES
    resolve.c resolve.h manifest.c manifest.h registry.c registry.h
    types.h sync.c sync.h main.c cli.c cli.h)

set(FORBIDDEN "\"gradle\"" "\"path\"" "\"npm\"" "daukle\\.source/[a-z]" "daukle\\.language/[a-z]")

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
