# Runs the commands whose code lives only in the executable: since the cli
# command files stopped being part of daukle_core, no test_*.c can link them,
# and a mutation in any of them turned nothing red (D-125's residual).

if(NOT DEFINED DAUKLE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR
            "check_cli_commands: usage is -DDAUKLE=<daukle binary> -DSOURCE_DIR=<repository> "
            "-DWORK_DIR=<scratch directory> -P cmake/check_cli_commands.cmake")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(ENV{DAUKLE_CACHE_DIR} "${WORK_DIR}/cache")

function(run_daukle directory expected_status output_variable)
    execute_process(COMMAND "${DAUKLE}" ${ARGN}
                    WORKING_DIRECTORY "${directory}"
                    OUTPUT_VARIABLE printed
                    ERROR_VARIABLE printed_error
                    RESULT_VARIABLE status)
    if(NOT status EQUAL expected_status)
        message(FATAL_ERROR "check_cli_commands: daukle ${ARGN} exited ${status}, expected "
                            "${expected_status}\n${printed}${printed_error}")
    endif()
    set(${output_variable} "${printed}${printed_error}" PARENT_SCOPE)
endfunction()

function(expect_text haystack needle what)
    string(FIND "${haystack}" "${needle}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "check_cli_commands: ${what} does not carry \"${needle}\":\n${haystack}")
    endif()
endfunction()

set(project_dir "${WORK_DIR}/project")
file(COPY "${SOURCE_DIR}/test/fixtures/modules-reach-generate/" DESTINATION "${project_dir}")

run_daukle("${project_dir}" 0 printed tasks)
expect_text("${printed}" "daukle: 1 task" "daukle tasks")
expect_text("${printed}" "stub:list" "daukle tasks")

run_daukle("${project_dir}" 0 printed config print)
expect_text("${printed}" "\"forebay/modules\"" "daukle config print")
expect_text("${printed}" "\"flavor\"" "daukle config print")
expect_text("${printed}" "daukle: plugins" "daukle config print")
expect_text("${printed}" "uses write" "daukle config print")

run_daukle("${project_dir}" 0 printed plugin update)
expect_text("${printed}" "daukle: no plugin here has a cache to clear" "daukle plugin update")

set(consumer_dir "${WORK_DIR}/consumer")
file(WRITE "${consumer_dir}/daukle.toml"
     "schema = 1\n"
     "project = \"forebay/x\"\n"
     "version = \"1.0.0\"\n"
     "\n"
     "# the comment that must survive\n"
     "[[consumers]]\n"
     "id = \"stub\"\n"
     "language = \"npm\"\n")

run_daukle("${consumer_dir}" 0 printed add "forebay/other@^1.0.0" --to stub --modules "core,api")
expect_text("${printed}" "daukle: added forebay/other@^1.0.0 to stub" "daukle add")
file(READ "${consumer_dir}/daukle.toml" edited)
expect_text("${edited}" "[consumers.dependencies.\"forebay/other\"]" "the manifest daukle add wrote")
expect_text("${edited}" "\"^1.0.0\"" "the manifest daukle add wrote")
expect_text("${edited}" "modules = [\"core\", \"api\"]" "the manifest daukle add wrote")
expect_text("${edited}" "# the comment that must survive" "the manifest daukle add wrote")

run_daukle("${consumer_dir}" 1 printed add "forebay/other@^1.0.0" --to absent)
expect_text("${printed}" "absent" "daukle add to a missing consumer")

# A published plugin is a tar, and a [plugins] path may name one, so this is
# the whole module path end to end: a real archive, one config print row per
# module, and plugin update taking the label that row shows (D-139).
set(tarred_dir "${WORK_DIR}/tarred")
set(tar_source "${WORK_DIR}/tarred-source")
file(COPY "${SOURCE_DIR}/test/fixtures/plugin-modules/plugins/plugin.lua" DESTINATION "${tar_source}")
file(COPY "${SOURCE_DIR}/test/fixtures/plugin-modules/plugins/modules/deps.lua"
     DESTINATION "${tar_source}/modules")
file(MAKE_DIRECTORY "${tarred_dir}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${tarred_dir}/modular.tar" --format=gnutar
                        plugin.lua modules/deps.lua
                WORKING_DIRECTORY "${tar_source}"
                RESULT_VARIABLE tar_status)
if(NOT tar_status EQUAL 0)
    message(FATAL_ERROR "check_cli_commands: could not build the plugin tar")
endif()

function(write_tarred_manifest modules)
    file(WRITE "${tarred_dir}/daukle.toml"
         "schema = 1\n"
         "project = \"forebay/tarred\"\n"
         "version = \"1.0.0\"\n"
         "\n"
         "[modules]\n"
         "\n"
         "[plugins]\n"
         "modular = { path = \"./modular.tar\", modules = ${modules} }\n")
endfunction()

write_tarred_manifest("[\"main\", \"deps\"]")
run_daukle("${tarred_dir}" 0 printed config print)
string(REGEX MATCH "  modular: [^\n]*" main_row "${printed}")
string(REGEX MATCH "  modular/deps: [^\n]*" deps_row "${printed}")
expect_text("${main_row}" "uses write" "config print's row for the main chunk")
expect_text("${deps_row}" "uses region" "config print's row for the deps module")

run_daukle("${tarred_dir}" 0 printed plugin update modular/deps)
expect_text("${printed}" "daukle: plugin \"modular/deps\" has no fetched artifact to clear"
            "daukle plugin update of a module row")
run_daukle("${tarred_dir}" 1 printed plugin update modular/absent)
expect_text("${printed}" "no plugin named \"modular/absent\"" "daukle plugin update of no module")

write_tarred_manifest("[\"nope\"]")
run_daukle("${tarred_dir}" 1 printed config print)
expect_text("${printed}" "plugin \"modular\": there is no module \"nope\": the plugin's modules are deps"
            "a missing module in a tar")

set(migrate_dir "${WORK_DIR}/migrate")
file(MAKE_DIRECTORY "${migrate_dir}")
unset(ENV{DAUKLE_CATALOGS})
run_daukle("${migrate_dir}" 1 printed migrate)
expect_text("${printed}" "DAUKLE_CATALOGS" "daukle migrate with no catalog")
run_daukle("${migrate_dir}" 1 printed migrate --done)
expect_text("${printed}" "no unfinished migration" "daukle migrate --done with nothing to finish")
run_daukle("${project_dir}" 2 printed tasks --catalog x)
expect_text("${printed}" "--catalog" "a migrate option given to tasks")

execute_process(COMMAND "${DAUKLE}" --version OUTPUT_VARIABLE version_line)
string(REGEX MATCH "[0-9][^\n\r ]*" self_version "${version_line}")
file(READ "${SOURCE_DIR}/wrapper/.daukle/wrapper.toml" pin_source)
string(REGEX REPLACE "\nversion = \"[^\"]*\"" "\nversion = \"${self_version}\"" own_pin "${pin_source}")
string(REGEX REPLACE "\nversion = \"[^\"]*\"" "\nversion = \"0.0.1-other\"" other_pin "${pin_source}")
file(WRITE "${WORK_DIR}/own-pin.toml" "${own_pin}")
file(WRITE "${WORK_DIR}/other-pin.toml" "${other_pin}")

function(expect_same_bytes actual expected what)
    file(SHA256 "${actual}" actual_digest)
    file(SHA256 "${expected}" expected_digest)
    if(NOT actual_digest STREQUAL expected_digest)
        message(FATAL_ERROR "check_cli_commands: ${what} is not byte for byte ${expected}")
    endif()
endfunction()

function(expect_absent path what)
    if(EXISTS "${path}")
        message(FATAL_ERROR "check_cli_commands: ${what} wrote ${path}")
    endif()
endfunction()

set(WRAPPER_FILES daukle daukle.ps1 .daukle/wrapper.toml)
set(ENV{DAUKLE_WRAPPER_PIN} "${WORK_DIR}/own-pin.toml")

set(init_dir "${WORK_DIR}/init-empty")
file(MAKE_DIRECTORY "${init_dir}")
run_daukle("${init_dir}" 0 printed init)
expect_text("${printed}" "wrote daukle.toml" "init in an empty directory")
expect_text("${printed}" "wrote .daukle/wrapper.toml" "init in an empty directory")
expect_same_bytes("${init_dir}/daukle" "${SOURCE_DIR}/wrapper/daukle" "the POSIX script init wrote")
expect_same_bytes("${init_dir}/daukle.ps1" "${SOURCE_DIR}/wrapper/daukle.ps1" "the PowerShell script init wrote")
expect_same_bytes("${init_dir}/.daukle/wrapper.toml" "${WORK_DIR}/own-pin.toml" "the pin init wrote")

set(init_dir "${WORK_DIR}/init-wrapper-present")
file(MAKE_DIRECTORY "${init_dir}/.daukle")
foreach(name ${WRAPPER_FILES})
    file(WRITE "${init_dir}/${name}" "kept ${name}\n")
endforeach()
run_daukle("${init_dir}" 0 printed init)
expect_text("${printed}" "wrote daukle.toml" "init beside a whole wrapper")
expect_text("${printed}" "wrapper update" "init beside a whole wrapper")
foreach(name ${WRAPPER_FILES})
    file(READ "${init_dir}/${name}" kept)
    if(NOT kept STREQUAL "kept ${name}\n")
        message(FATAL_ERROR "check_cli_commands: init rewrote ${name} beside a whole wrapper")
    endif()
endforeach()

set(init_dir "${WORK_DIR}/init-part-present")
file(MAKE_DIRECTORY "${init_dir}")
file(WRITE "${init_dir}/daukle.ps1" "kept\n")
run_daukle("${init_dir}" 0 printed init)
expect_text("${printed}" "part of the wrapper is already here" "init beside part of a wrapper")
expect_absent("${init_dir}/daukle" "init beside part of a wrapper")
expect_absent("${init_dir}/.daukle/wrapper.toml" "init beside part of a wrapper")

set(init_dir "${WORK_DIR}/init-all-present")
file(MAKE_DIRECTORY "${init_dir}/.daukle")
foreach(name ${WRAPPER_FILES} daukle.toml)
    file(WRITE "${init_dir}/${name}" "kept\n")
endforeach()
run_daukle("${init_dir}" 2 printed init)
foreach(name ${WRAPPER_FILES} daukle.toml)
    file(READ "${init_dir}/${name}" kept)
    if(NOT kept STREQUAL "kept\n")
        message(FATAL_ERROR "check_cli_commands: init rewrote ${name} when everything was there")
    endif()
endforeach()

set(ENV{DAUKLE_WRAPPER_PIN} "${WORK_DIR}/other-pin.toml")
set(init_dir "${WORK_DIR}/init-other-version")
file(MAKE_DIRECTORY "${init_dir}")
run_daukle("${init_dir}" 3 printed init)
expect_text("${printed}" "pins daukle \"0.0.1-other\" but this is daukle ${self_version}"
            "init with another version's pin")
expect_text("${printed}" "other-pin.toml" "init with another version's pin")
foreach(name ${WRAPPER_FILES})
    expect_absent("${init_dir}/${name}" "init with another version's pin")
endforeach()

unset(ENV{DAUKLE_WRAPPER_PIN})
get_filename_component(daukle_directory "${DAUKLE}" DIRECTORY)
if(EXISTS "${daukle_directory}/wrapper.toml" OR EXISTS "${daukle_directory}/../share/daukle/wrapper.toml")
    message(FATAL_ERROR "check_cli_commands: a wrapper.toml sits where init looks beside "
                        "${DAUKLE}, so the no-pin case cannot be tested")
endif()
set(init_dir "${WORK_DIR}/init-no-pin")
file(MAKE_DIRECTORY "${init_dir}")
run_daukle("${init_dir}" 3 printed init)
expect_text("${printed}" "wrote daukle.toml" "init with no pin")
expect_text("${printed}" "share/daukle/wrapper.toml" "init with no pin")
foreach(name ${WRAPPER_FILES})
    expect_absent("${init_dir}/${name}" "init with no pin")
endforeach()

message(STATUS "check_cli_commands: tasks, config print, plugin update, add, migrate and init all ran")
