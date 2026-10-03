# Refuses a binary whose --version disagrees with what it is being published as.
#
# Two callers. The ctest entry version_matches_build passes the project's own
# VERSION, so a --version that stops tracking the build reddens the suite rather
# than shipping. A publish passes the tag, which is the check that matters: a
# wrapper verifies a downloaded daukle by running it, so a tag and a binary that
# disagree make every such verification wrong.
#
# It runs the BINARY rather than reading CMakeLists.txt, because what a release
# publishes is the artifact, and a source file is not evidence about it.

if(NOT DEFINED DAUKLE OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR
            "check_version: usage is -DDAUKLE=<path to the daukle binary> "
            "-DEXPECTED=<version or tag> -P cmake/check_version.cmake")
endif()

# Tags in this org are bare, but a leading v costs nothing to accept and a
# refusal over tag STYLE would say the versions disagree, which is a lie.
string(REGEX REPLACE "^v" "" EXPECTED "${EXPECTED}")

execute_process(COMMAND "${DAUKLE}" --version
                OUTPUT_VARIABLE printed
                ERROR_VARIABLE printed_error
                RESULT_VARIABLE status)

if(NOT status EQUAL 0)
    message(FATAL_ERROR "check_version: ${DAUKLE} --version exited ${status}: ${printed_error}")
endif()

# Windows prints CRLF and string(STRIP) is not specified to drop the CR.
string(REGEX REPLACE "[ \t\r\n]+$" "" printed "${printed}")

if(NOT printed STREQUAL "daukle ${EXPECTED}")
    message(FATAL_ERROR
            "check_version: the binary prints \"${printed}\" and it is being published as "
            "\"${EXPECTED}\". Raise VERSION in project() to match the tag, or tag the version "
            "that was built.")
endif()

message(STATUS "check_version: daukle ${EXPECTED}")
