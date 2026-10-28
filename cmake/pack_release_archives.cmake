# Packs one archive per host of the release's binary and its wrapper.toml, which is what every
# install channel consumes: `daukle init` finds the pin beside the executable. Run from the
# release's `finalize`, after write_wrapper_pin.cmake and before the scripts are copied in, so
# the only files in ASSET_DIR are the binaries and the pin.
#
# The bare binaries stay assets, because every committed project pin names them.

if(NOT DEFINED ASSET_DIR)
    message(FATAL_ERROR "pack_release_archives: usage is -DASSET_DIR=<collected assets> "
                        "-P cmake/pack_release_archives.cmake")
endif()

set(pin "${ASSET_DIR}/wrapper.toml")
if(NOT EXISTS "${pin}")
    message(FATAL_ERROR "pack_release_archives: ${pin} does not exist; write the pin first")
endif()

file(GLOB binaries LIST_DIRECTORIES false "${ASSET_DIR}/daukle-*")
set(packed 0)
foreach(binary ${binaries})
    get_filename_component(asset "${binary}" NAME)
    if(asset MATCHES "\\.(tar\\.gz|zip)$")
        continue()
    endif()

    if(asset MATCHES "\\.exe$")
        string(REGEX REPLACE "\\.exe$" ".zip" archive "${asset}")
        set(member daukle.exe)
        set(format zip)
    else()
        set(archive "${asset}.tar.gz")
        set(member daukle)
        set(format gnutar)
    endif()

    # A downloaded artifact has lost its mode, so the member is staged with one rather than
    # archived as it arrived.
    set(stage "${ASSET_DIR}/../pack-${asset}")
    file(REMOVE_RECURSE "${stage}")
    file(MAKE_DIRECTORY "${stage}")
    file(COPY_FILE "${binary}" "${stage}/${member}")
    file(CHMOD "${stage}/${member}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
               GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    file(COPY_FILE "${pin}" "${stage}/wrapper.toml")

    if(format STREQUAL "zip")
        set(flags cf)
    else()
        set(flags czf)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar ${flags} "${ASSET_DIR}/${archive}"
                            --format=${format} "${member}" wrapper.toml
                    WORKING_DIRECTORY "${stage}"
                    RESULT_VARIABLE status)
    file(REMOVE_RECURSE "${stage}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "pack_release_archives: could not pack ${archive}")
    endif()
    message(STATUS "pack_release_archives: ${archive} holds ${member} and wrapper.toml")
    math(EXPR packed "${packed} + 1")
endforeach()

# A glob that matched nothing reads exactly like a clean run.
if(packed LESS 3)
    message(FATAL_ERROR "pack_release_archives: packed ${packed} archives, fewer than the three "
                        "hosts a release builds, so the asset names have drifted from this script")
endif()
