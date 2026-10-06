# Refuses a wrapper-bootstrap example whose copies of the wrapper have drifted.
#
# The example ships `daukle`, `daukle.ps1` and `.daukle/wrapper.toml` so that it
# owns no daukle and is run by the one its own wrapper fetches. All three are
# copies of wrapper/, and a copy that drifts demonstrates something nobody
# ships.
#
# This check could not exist while the two lived in separate repositories: the
# example's pin could legitimately name a release older than core's working
# tree, so the comparison had no right answer and the example's own ABOUT.md
# said the pin was deliberately not compared. In one repository a release
# updates both in the same commit, so all three files compare.

if(NOT DEFINED WRAPPER OR NOT DEFINED COPY)
    message(FATAL_ERROR
            "check_wrapper_copy: usage is -DWRAPPER=<dir> -DCOPY=<dir> "
            "-P cmake/check_wrapper_copy.cmake")
endif()

set(FILES daukle daukle.ps1 .daukle/wrapper.toml)
set(DRIFTED "")
set(COMPARED 0)

foreach(relative ${FILES})
    set(left "${WRAPPER}/${relative}")
    set(right "${COPY}/${relative}")
    if(NOT EXISTS "${left}")
        list(APPEND DRIFTED "${relative}: ${left} does not exist")
        continue()
    endif()
    if(NOT EXISTS "${right}")
        list(APPEND DRIFTED "${relative}: ${right} does not exist")
        continue()
    endif()
    file(SHA256 "${left}" left_digest)
    file(SHA256 "${right}" right_digest)
    if(NOT left_digest STREQUAL right_digest)
        list(APPEND DRIFTED "${relative}: ${left_digest} in wrapper/, ${right_digest} in the example")
    endif()
    math(EXPR COMPARED "${COMPARED} + 1")
endforeach()

# A loop that compared nothing reads exactly like a clean run, which is the bug
# every other check in this directory has shipped at least once.
if(COMPARED LESS 3 AND NOT DRIFTED)
    message(FATAL_ERROR "check_wrapper_copy: compared ${COMPARED} files of 3, so the check has gone stale")
endif()

if(DRIFTED)
    foreach(finding ${DRIFTED})
        message(STATUS "check_wrapper_copy: ${finding}")
    endforeach()
    message(FATAL_ERROR "the wrapper-bootstrap example no longer carries wrapper/ byte for byte")
endif()

message(STATUS "check_wrapper_copy: ${COMPARED} files, byte identical to wrapper/")
