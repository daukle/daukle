# Refuses a macOS daukle that links anything outside the OS.
#
# macOS is the one host cell that cannot be static: there is no static
# libSystem, so DAUKLE_STATIC is a FATAL_ERROR there and a published macOS
# binary is dynamically linked by construction. Measured on a hosted runner
# 2026-10-03, find_package(CURL) resolves /usr/lib/libcurl.4.dylib and the whole
# link set is that plus libSystem, which makes macOS self-contained like Windows
# rather than a prerequisite like Linux was.
#
# That is a property of the RUNNER IMAGE, not of this project, so it is a test
# rather than a sentence in a document: a Homebrew libcurl appearing on a future
# image would otherwise turn a published binary into a download and a package
# manager, silently.
#
# Registered only on APPLE, so the other two runners report no skip.

if(NOT DEFINED DAUKLE)
    message(FATAL_ERROR "check_macos_links: usage is -DDAUKLE=<path to the daukle binary> "
                        "-P cmake/check_macos_links.cmake")
endif()

execute_process(COMMAND otool -L "${DAUKLE}"
                OUTPUT_VARIABLE listing
                ERROR_VARIABLE listing_error
                RESULT_VARIABLE status)

if(NOT status EQUAL 0)
    message(FATAL_ERROR "check_macos_links: otool -L ${DAUKLE} exited ${status}: ${listing_error}")
endif()

string(REPLACE "\r" "" listing "${listing}")
string(REPLACE "\n" ";" lines "${listing}")

# The first line is the binary's own path followed by a colon, and a dependency
# line is indented. Dropping by INDEX would also drop a dependency if otool ever
# stopped printing that header, so the header is matched rather than counted.
set(FOREIGN "")
set(DEPENDENCIES_SEEN 0)
foreach(line ${lines})
    if(NOT line MATCHES "^[ \t]")
        continue()
    endif()
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    math(EXPR DEPENDENCIES_SEEN "${DEPENDENCIES_SEEN} + 1")
    if(NOT line MATCHES "^(/usr/lib/|/System/)")
        list(APPEND FOREIGN "${line}")
    endif()
endforeach()

# A listing that parsed into nothing reads exactly like a clean run, which this
# repository's other checks have shipped twice.
if(DEPENDENCIES_SEEN LESS 2)
    message(FATAL_ERROR "check_macos_links: parsed ${DEPENDENCIES_SEEN} dependency lines out of "
                        "otool, which is too few to be a real link set; the parse has gone stale")
endif()

if(FOREIGN)
    foreach(entry ${FOREIGN})
        message(STATUS "check_macos_links: ${entry}")
    endforeach()
    message(FATAL_ERROR
            "a published macOS daukle must link only /usr/lib and /System, and this one does not. "
            "A Homebrew libcurl makes the binary a download and a package manager rather than a "
            "download.")
endif()

message(STATUS "check_macos_links: ${DEPENDENCIES_SEEN} dependencies, all under /usr/lib or /System")
