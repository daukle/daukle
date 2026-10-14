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

message(STATUS "check_cli_commands: tasks, config print, plugin update and add all ran")
