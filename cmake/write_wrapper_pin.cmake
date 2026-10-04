# Writes the wrapper pin for a release, from the assets that release carries.
#
# Digests exist for the first time in the release job, after every cell has
# uploaded and before anything is published, which is the one moment this file
# can be produced rather than typed. It is invoked from publish-matrix.yml's
# `finalize`. D-67.
#
# It EDITS the pin rather than rendering one: only the version line and the
# sha256 lines change, so the file's comments, its host keys and its asset names
# survive. Those three are design, and a generator that re-emitted them would
# own them.
#
# The host-to-asset mapping is therefore still hand written, and that is
# deliberate: it is the one part of the pin a release cannot infer, since
# nothing in "daukle-macos-arm64" says it serves macos/aarch64. What it CANNOT
# do is go stale quietly, because an asset no host names is a failure below.

if(NOT DEFINED PIN OR NOT DEFINED ASSET_DIR OR NOT DEFINED VERSION OR NOT DEFINED OUT)
    message(FATAL_ERROR
            "write_wrapper_pin: usage is -DPIN=<path to wrapper.toml> -DASSET_DIR=<collected "
            "assets> -DVERSION=<tag> -DOUT=<path to write> -P cmake/write_wrapper_pin.cmake")
endif()

string(REGEX REPLACE "^v" "" VERSION "${VERSION}")

file(READ "${PIN}" pin_text)
string(REPLACE ";" "\\;" pin_text "${pin_text}")
string(REPLACE "\r" "" pin_text "${pin_text}")

# Every line is prefixed with a marker before the split because foreach SKIPS an
# empty list item, which silently deleted all four blank lines from the rendered
# pin. The marker makes an empty line the one-character item "|" instead.
string(REPLACE "\n" ";|" pin_lines "|${pin_text}")
if(pin_text MATCHES "\n$")
    # The trailing newline ends the last line rather than starting another, so
    # the split leaves one marker that is not a line.
    list(POP_BACK pin_lines)
endif()

# Listed before anything is written, so OUT landing inside ASSET_DIR cannot be
# mistaken for an asset nobody pinned.
file(GLOB PRESENT_ASSETS "${ASSET_DIR}/*")

set(PIN_HOSTS "")
set(host "")
foreach(marked ${pin_lines})
    string(SUBSTRING "${marked}" 1 -1 line)
    string(STRIP "${line}" stripped)
    if(stripped MATCHES "^\\[assets\\.\"([^\"]+)\"\\]$")
        set(host "${CMAKE_MATCH_1}")
    elseif(stripped MATCHES "^asset[ \t]*=[ \t]*\"([^\"]+)\"$" AND NOT host STREQUAL "")
        list(APPEND PIN_HOSTS "${host}")
        set(PIN_ASSET_${host} "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(LENGTH PIN_HOSTS PIN_HOST_COUNT)
if(PIN_HOST_COUNT LESS 3)
    message(FATAL_ERROR "write_wrapper_pin: parsed ${PIN_HOST_COUNT} host entries out of ${PIN}, "
                        "which is too few to be the real pin; the parse has gone stale")
endif()

set(CLAIMED "")
foreach(host ${PIN_HOSTS})
    set(asset "${PIN_ASSET_${host}}")
    set(path "${ASSET_DIR}/${asset}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "write_wrapper_pin: the pin's ${host} names ${asset} and this release "
                            "carries no such asset, so the wrapper would 404 on that host")
    endif()
    file(SHA256 "${path}" SHA_${host})
    list(APPEND CLAIMED "${path}")
    message(STATUS "write_wrapper_pin: ${host} ${asset} ${SHA_${host}}")
endforeach()

# The direction the pin cannot see. A release that grew a platform leaves the
# wrapper refusing that host by name, which reads to its user like an
# unsupported platform rather than a mapping nobody added.
foreach(present ${PRESENT_ASSETS})
    list(FIND CLAIMED "${present}" claimed_at)
    if(claimed_at EQUAL -1)
        get_filename_component(present_name "${present}" NAME)
        message(FATAL_ERROR "write_wrapper_pin: this release carries ${present_name} and no host "
                            "entry in ${PIN} names it, so the wrapper cannot serve that platform")
    endif()
endforeach()

set(rendered "")
set(host "")
set(VERSION_WRITTEN 0)
set(DIGESTS_WRITTEN 0)
foreach(marked ${pin_lines})
    string(SUBSTRING "${marked}" 1 -1 line)
    string(STRIP "${line}" stripped)
    if(stripped MATCHES "^\\[assets\\.\"([^\"]+)\"\\]$")
        set(host "${CMAKE_MATCH_1}")
    elseif(stripped MATCHES "^version[ \t]*=" AND host STREQUAL "")
        set(line "version = \"${VERSION}\"")
        math(EXPR VERSION_WRITTEN "${VERSION_WRITTEN} + 1")
    elseif(stripped MATCHES "^sha256[ \t]*=" AND NOT host STREQUAL "")
        set(line "sha256 = \"${SHA_${host}}\"")
        math(EXPR DIGESTS_WRITTEN "${DIGESTS_WRITTEN} + 1")
    endif()
    string(APPEND rendered "${line}\n")
endforeach()

# A rewrite that replaced nothing produces a valid file naming the PREVIOUS
# release, which is the exact failure this script exists to delete and the one
# shape of it that would still publish.
if(NOT VERSION_WRITTEN EQUAL 1 OR NOT DIGESTS_WRITTEN EQUAL PIN_HOST_COUNT)
    message(FATAL_ERROR "write_wrapper_pin: replaced ${VERSION_WRITTEN} version lines and "
                        "${DIGESTS_WRITTEN} of ${PIN_HOST_COUNT} digests, so the rewrite has gone "
                        "stale and the pin would name the previous release")
endif()

string(REPLACE "\\;" ";" rendered "${rendered}")

# file(CONFIGURE) rather than file(WRITE), for NEWLINE_STYLE alone. file(WRITE)
# opens in text mode on Windows and turns every LF into CRLF, which would
# publish a pin differing from the committed one by line ending only and redden
# the drift check in daukle/examples. The release job runs on Linux, where the
# bug cannot fire, which is the whole reason to pin it here rather than rely on
# it: measured on Windows, where it does.
#
# @ONLY narrows substitution to @VAR@, and the guard makes an @ in the pin a
# refusal rather than a quiet replacement.
if(rendered MATCHES "@")
    message(FATAL_ERROR "write_wrapper_pin: the pin contains an @, which file(CONFIGURE) would "
                        "try to substitute. Write it with another mechanism.")
endif()
file(CONFIGURE OUTPUT "${OUT}" CONTENT "${rendered}" @ONLY NEWLINE_STYLE LF)

message(STATUS "write_wrapper_pin: wrote ${OUT} for ${VERSION}, ${PIN_HOST_COUNT} hosts")
