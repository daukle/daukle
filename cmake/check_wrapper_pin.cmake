# Refuses a wrapper pin that no longer describes the latest published release.
#
# The pin in wrapper/.daukle/wrapper.toml decides what a project without daukle
# downloads, and before this check nothing failed when it went stale: the
# wrapper's own probe tests whatever the pin says, and daukle/examples pins the
# same version, so a forgotten bump stayed green everywhere. D-67.
#
# It asks the forge rather than reading a local file, because staleness is a
# disagreement between this repository and what is published, and no file here
# is evidence about that.
#
# No asset is downloaded: the releases API reports a sha256 per asset, which is
# the same number the pin carries. A 2 KB fetch runs on every CI run on three
# runners where 26 MB of binaries could not.

if(NOT DEFINED PIN OR NOT DEFINED REPO)
    message(FATAL_ERROR
            "check_wrapper_pin: usage is -DPIN=<path to wrapper.toml> -DREPO=<owner/name> "
            "-P cmake/check_wrapper_pin.cmake")
endif()

# ctest reads this as Skipped through SKIP_RETURN_CODE. Offline is the one
# answer that is neither pass nor fail, and it has to stay visible: a check that
# could not reach the forge and said nothing is the silent pass this file exists
# to delete.
set(SKIP_EXIT 77)

if(NOT DEFINED ENV{DAUKLE_NETWORK_TESTS})
    message(STATUS "check_wrapper_pin: skipped, DAUKLE_NETWORK_TESTS is not set")
    cmake_language(EXIT ${SKIP_EXIT})
endif()

file(READ "${PIN}" pin_text)

# Semicolons are escaped before the split and brackets are not: foreach expands
# on ';' alone, and [assets."linux/x86_64"] is balanced. A stray ';' in a
# comment would otherwise shred one line into two that match nothing.
string(REPLACE ";" "\\;" pin_text "${pin_text}")
string(REPLACE "\r" "" pin_text "${pin_text}")
string(REPLACE "\n" ";" pin_lines "${pin_text}")

set(PIN_VERSION "")
set(PIN_HOSTS "")
set(host "")
set(asset "")
foreach(line ${pin_lines})
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    if(line MATCHES "^\\[assets\\.\"([^\"]+)\"\\]$")
        set(host "${CMAKE_MATCH_1}")
        set(asset "")
    elseif(line MATCHES "^version[ \t]*=[ \t]*\"([^\"]+)\"$" AND host STREQUAL "")
        set(PIN_VERSION "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^asset[ \t]*=[ \t]*\"([^\"]+)\"$")
        set(asset "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^sha256[ \t]*=[ \t]*\"([0-9a-f]+)\"$")
        if(host STREQUAL "" OR asset STREQUAL "")
            message(FATAL_ERROR "check_wrapper_pin: a sha256 appears before any [assets.\"...\"] "
                                "and asset, so the pin does not parse")
        endif()
        list(APPEND PIN_HOSTS "${host}")
        set(PIN_ASSET_${host} "${asset}")
        set(PIN_SHA_${host} "${CMAKE_MATCH_1}")
    endif()
endforeach()

# A parse that found nothing reads exactly like a clean run, and every other
# check in this directory has shipped that bug at least once.
list(LENGTH PIN_HOSTS PIN_HOST_COUNT)
if(PIN_VERSION STREQUAL "" OR PIN_HOST_COUNT LESS 3)
    message(FATAL_ERROR "check_wrapper_pin: parsed version \"${PIN_VERSION}\" and "
                        "${PIN_HOST_COUNT} host entries out of ${PIN}, which is too little to be "
                        "the real pin; the parse has gone stale")
endif()

set(API "https://api.github.com/repos/${REPO}/releases/latest")

# In script mode CMAKE_CURRENT_BINARY_DIR is simply the working directory, so an
# unqualified default drops the response into whatever tree the caller happened
# to be standing in. It is removed again below, and the ctest entry names a path
# under build/ regardless.
if(NOT DEFINED RESPONSE)
    set(RESPONSE "${CMAKE_CURRENT_BINARY_DIR}/wrapper-pin-latest.json")
endif()

# Unauthenticated on purpose. The ctest step this runs in is given no token, and
# taking one would mean a second change to another org's test workflow for a
# 2 KB public read. The rate limit that buys is handled below rather than
# ignored.
file(DOWNLOAD "${API}" "${RESPONSE}"
     HTTPHEADER "Accept: application/vnd.github+json"
     HTTPHEADER "X-GitHub-Api-Version: 2022-11-28"
     STATUS download_status
     LOG download_log)
list(GET download_status 1 download_message)

# The HTTP code rather than the curl one, and the LAST of them because a
# redirect leaves two status lines in the log. file(DOWNLOAD) reports every
# HTTP error as the same code 22, which made a repository with no release at
# all skip as "unreachable": the one answer this check must never give for a
# question the forge did answer.
set(HTTP_CODE "")
string(REGEX MATCHALL "HTTP/[0-9.]+ ([0-9][0-9][0-9])" status_lines "${download_log}")
if(status_lines)
    list(GET status_lines -1 last_status)
    string(REGEX REPLACE "^HTTP/[0-9.]+ " "" HTTP_CODE "${last_status}")
endif()

if(HTTP_CODE STREQUAL "")
    message(STATUS "check_wrapper_pin: skipped, ${API} answered nothing: ${download_message}")
    cmake_language(EXIT ${SKIP_EXIT})
endif()

# A rate limit is a property of the runner's shared address and says nothing
# about the pin, so it is the third answer rather than a red. Everything else
# the forge says, 404 included, is about this repository and is a failure.
if(HTTP_CODE STREQUAL "403" OR HTTP_CODE STREQUAL "429")
    message(STATUS "check_wrapper_pin: skipped, ${API} answered ${HTTP_CODE}, which is its rate limit")
    cmake_language(EXIT ${SKIP_EXIT})
endif()

if(NOT HTTP_CODE STREQUAL "200")
    message(FATAL_ERROR
            "check_wrapper_pin: ${API} answered ${HTTP_CODE}. A 404 here means ${REPO} has no "
            "published release, so there is nothing for the wrapper to pin to.")
endif()

file(READ "${RESPONSE}" body)
file(REMOVE "${RESPONSE}")

string(JSON LATEST_TAG ERROR_VARIABLE tag_error GET "${body}" "tag_name")
if(tag_error)
    message(FATAL_ERROR "check_wrapper_pin: ${API} returned no tag_name: ${tag_error}")
endif()

string(REGEX REPLACE "^v" "" LATEST_TAG "${LATEST_TAG}")
string(REGEX REPLACE "^v" "" PIN_VERSION_BARE "${PIN_VERSION}")

if(NOT PIN_VERSION_BARE STREQUAL LATEST_TAG)
    message(FATAL_ERROR
            "check_wrapper_pin: the pin names ${PIN_VERSION} and the latest ${REPO} release is "
            "${LATEST_TAG}. A project that copies ${PIN} installs ${PIN_VERSION} and nothing tells "
            "it so. Regenerate the pin from the ${LATEST_TAG} assets.")
endif()

string(JSON ASSET_COUNT ERROR_VARIABLE asset_error LENGTH "${body}" "assets")
if(asset_error)
    message(FATAL_ERROR "check_wrapper_pin: ${API} returned no assets array: ${asset_error}")
endif()

set(FORGE_NAMES "")
set(MISMATCHES "")
math(EXPR LAST_ASSET "${ASSET_COUNT} - 1")
foreach(index RANGE 0 ${LAST_ASSET})
    string(JSON name GET "${body}" "assets" ${index} "name")
    list(APPEND FORGE_NAMES "${name}")
    string(JSON digest ERROR_VARIABLE digest_error GET "${body}" "assets" ${index} "digest")

    # Reported as a failure rather than worked around by downloading the asset.
    # A fallback here would be the branch that only ever runs when the forge
    # changes, which is the branch nobody reviews, and "I could not tell" must
    # not reach the caller as "they agree".
    if(digest_error OR NOT digest MATCHES "^sha256:([0-9a-f]+)$")
        list(APPEND MISMATCHES
             "${name}: the forge reported no sha256 digest, so this check cannot verify it")
        continue()
    endif()
    set(FORGE_SHA_${name} "${CMAKE_MATCH_1}")
endforeach()

foreach(host ${PIN_HOSTS})
    set(asset "${PIN_ASSET_${host}}")
    if(NOT DEFINED FORGE_SHA_${asset})
        list(APPEND MISMATCHES
             "${host}: the pin names ${asset} and release ${LATEST_TAG} carries no such file")
    elseif(NOT PIN_SHA_${host} STREQUAL FORGE_SHA_${asset})
        list(APPEND MISMATCHES
             "${host}: the pin says ${PIN_SHA_${host}} and ${asset} in ${LATEST_TAG} is ${FORGE_SHA_${asset}}")
    endif()
endforeach()

# The other direction, which the pin cannot notice on its own: a release that
# grew a platform leaves the wrapper refusing that host by name, which reads
# like an unsupported host rather than a pin nobody updated.
foreach(name ${FORGE_NAMES})
    set(claimed 0)
    foreach(host ${PIN_HOSTS})
        if(PIN_ASSET_${host} STREQUAL "${name}")
            set(claimed 1)
        endif()
    endforeach()
    if(NOT claimed)
        list(APPEND MISMATCHES
             "${name} is published in ${LATEST_TAG} and no pin entry names it, so the wrapper refuses that host")
    endif()
endforeach()

if(MISMATCHES)
    foreach(finding ${MISMATCHES})
        message(STATUS "check_wrapper_pin: ${finding}")
    endforeach()
    message(FATAL_ERROR "the wrapper pin disagrees with the latest ${REPO} release")
endif()

message(STATUS "check_wrapper_pin: ${PIN_VERSION}, ${PIN_HOST_COUNT} hosts, every digest matches "
               "release ${LATEST_TAG}")
