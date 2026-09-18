cmake_minimum_required(VERSION 3.20)
project(daukle C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    set(DAUKLE_WARNING_FLAGS /W4 /WX /D_CRT_SECURE_NO_WARNINGS)
    # greatest.h's assertion macros expand literal-constant comparisons into an
    # `if`, which MSVC's C4127 flags even though the do/while(0) wrapper is fine.
    # A leading SKIPm() also returns unconditionally, so MSVC's C4702 flags the
    # rest of that test body even though skipping the whole test is intentional.
    set(DAUKLE_TEST_WARNING_FLAGS ${DAUKLE_WARNING_FLAGS} /wd4127 /wd4702)
else()
    set(DAUKLE_WARNING_FLAGS -Wall -Wextra -Werror)
    set(DAUKLE_TEST_WARNING_FLAGS ${DAUKLE_WARNING_FLAGS})
endif()

add_library(daukle_vendor STATIC vendor/cJSON/cJSON.c)
target_include_directories(daukle_vendor PUBLIC vendor/cJSON)

file(GLOB DAUKLE_CORE_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM DAUKLE_CORE_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
     ${CMAKE_CURRENT_SOURCE_DIR}/src/http_winhttp.c
     ${CMAKE_CURRENT_SOURCE_DIR}/src/http_curl.c)

if(WIN32)
    list(APPEND DAUKLE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/http_winhttp.c)
else()
    list(APPEND DAUKLE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/http_curl.c)
endif()

add_library(daukle_core STATIC ${DAUKLE_CORE_SOURCES})
target_compile_options(daukle_core PRIVATE ${DAUKLE_WARNING_FLAGS})
target_include_directories(daukle_core PUBLIC src vendor/cJSON)
target_link_libraries(daukle_core PUBLIC daukle_vendor)

if(WIN32)
    target_link_libraries(daukle_core PUBLIC winhttp)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(daukle_core PUBLIC CURL::libcurl)
endif()

add_executable(daukle src/main.c)
target_link_libraries(daukle PRIVATE daukle_core)
target_compile_options(daukle PRIVATE ${DAUKLE_WARNING_FLAGS})

enable_testing()
add_library(daukle_test_support STATIC test/support.c test/http_server.c)
target_include_directories(daukle_test_support PUBLIC test)
target_compile_options(daukle_test_support PRIVATE ${DAUKLE_WARNING_FLAGS})

if(WIN32)
    target_link_libraries(daukle_test_support PUBLIC ws2_32)
else()
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(daukle_test_support PUBLIC Threads::Threads)
endif()

file(GLOB DAUKLE_TESTS CONFIGURE_DEPENDS test/test_*.c)
foreach(test_file ${DAUKLE_TESTS})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_executable(${test_name} ${test_file})
    target_include_directories(${test_name} PRIVATE vendor/greatest test)
    target_link_libraries(${test_name} PRIVATE daukle_core daukle_test_support)
    target_compile_options(${test_name} PRIVATE ${DAUKLE_TEST_WARNING_FLAGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

add_test(NAME agnostic_core COMMAND ${CMAKE_COMMAND}
         -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
         -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_agnostic.cmake)
