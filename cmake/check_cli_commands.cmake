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

message(STATUS "check_cli_commands: tasks, config print, plugin update, add and migrate all ran")
