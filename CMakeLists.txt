cmake_minimum_required(VERSION 3.20)
project(daukle C)

# A published daukle must run where nothing is installed, and libcurl.so.4 is
# absent from every bare Linux image including the one it was built on. OFF
# everywhere but a release build, where it is what makes the artifact a
# download rather than a download and a package manager.
option(DAUKLE_STATIC "Link the daukle executable with no shared library dependencies" OFF)

if(DAUKLE_STATIC AND APPLE)
    message(FATAL_ERROR
            "DAUKLE_STATIC is not available on macOS: there is no static libSystem, so a fully "
            "static binary cannot be produced. A macOS release links the system libcurl, which "
            "unlike Linux is present on every host.")
endif()

if(DAUKLE_STATIC AND MSVC)
    # The dynamic CRT puts VCRUNTIME140.dll in the import table, and that ships
    # with the Visual C++ redistributable rather than with Windows.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Stated rather than inherited: OFF means -std=c11, which puts glibc into
# __STRICT_ANSI__ and hides symlink and chmod from src/unpack.c and ftruncate
# and fileno from src/net/http_curl.c.
set(CMAKE_C_EXTENSIONS ON)

if(MSVC)
    set(DAUKLE_WARNING_FLAGS /W4 /WX /D_CRT_SECURE_NO_WARNINGS)
    # greatest.h's assertion macros expand literal-constant comparisons into an
    # `if`, which MSVC's C4127 flags even though the do/while(0) wrapper is fine.
    set(DAUKLE_TEST_WARNING_FLAGS ${DAUKLE_WARNING_FLAGS} /wd4127)
else()
    set(DAUKLE_WARNING_FLAGS -Wall -Wextra -Werror)
    set(DAUKLE_TEST_WARNING_FLAGS ${DAUKLE_WARNING_FLAGS})
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # An assertion message composed into a fixed buffer is still a correct
        # assertion message when it truncates. src/ keeps the diagnostic, where
        # a truncated path is a defect rather than a shorter sentence.
        list(APPEND DAUKLE_TEST_WARNING_FLAGS -Wno-format-truncation)
    endif()
endif()

file(GLOB DAUKLE_LUA_SOURCES CONFIGURE_DEPENDS vendor/lua/*.c)
add_library(daukle_vendor STATIC vendor/cJSON/cJSON.c vendor/toml/toml.c vendor/miniz/miniz.c
            ${DAUKLE_LUA_SOURCES})
target_include_directories(daukle_vendor PUBLIC vendor/cJSON vendor/toml vendor/lua vendor/miniz)
# miniz decompresses and enumerates; every path decision and every write is
# daukle's. Without stdio it cannot open or create a file even by mistake.
target_compile_definitions(daukle_vendor PUBLIC MINIZ_NO_STDIO)
if(NOT WIN32)
    target_compile_definitions(daukle_vendor PRIVATE LUA_USE_POSIX)
    target_link_libraries(daukle_vendor PUBLIC m)
endif()

file(GLOB DAUKLE_CORE_SOURCES CONFIGURE_DEPENDS src/*/*.c)
list(REMOVE_ITEM DAUKLE_CORE_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/main.c
     ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_winhttp.c
     ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_curl.c
     ${CMAKE_CURRENT_SOURCE_DIR}/src/exec/exec_win32.c
     ${CMAKE_CURRENT_SOURCE_DIR}/src/exec/exec_posix.c)

if(WIN32)
    list(APPEND DAUKLE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_winhttp.c)
    list(APPEND DAUKLE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/exec/exec_win32.c)
else()
    list(APPEND DAUKLE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/net/http_curl.c)
    list(APPEND DAUKLE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/exec/exec_posix.c)
endif()

add_library(daukle_core STATIC ${DAUKLE_CORE_SOURCES})
target_compile_options(daukle_core PRIVATE ${DAUKLE_WARNING_FLAGS})
target_include_directories(daukle_core PUBLIC src vendor/cJSON)
target_link_libraries(daukle_core PUBLIC daukle_vendor)

if(WIN32)
    target_link_libraries(daukle_core PUBLIC winhttp)
elseif(DAUKLE_STATIC)
    # pkg-config is the only thing that knows libcurl's static dependency
    # closure, and FindCURL resolves the shared object even when asked for the
    # static one: it reports success and the link then fails with "attempted
    # static link of dynamic object".
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBCURL REQUIRED libcurl)
    target_include_directories(daukle_core PUBLIC ${LIBCURL_STATIC_INCLUDE_DIRS})
    target_link_directories(daukle_core PUBLIC ${LIBCURL_STATIC_LIBRARY_DIRS})
    target_link_libraries(daukle_core PUBLIC ${LIBCURL_STATIC_LIBRARIES})
    target_compile_definitions(daukle_core PRIVATE DAUKLE_STATIC_CURL)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(daukle_core PUBLIC CURL::libcurl)
endif()

add_executable(daukle src/cli/main.c)
target_link_libraries(daukle PRIVATE daukle_core)
target_compile_options(daukle PRIVATE ${DAUKLE_WARNING_FLAGS})
if(DAUKLE_STATIC AND NOT WIN32)
    target_link_options(daukle PRIVATE -static)
endif()

enable_testing()
add_library(daukle_test_support STATIC test/support.c test/http_server.c)
target_include_directories(daukle_test_support PUBLIC test)
target_compile_options(daukle_test_support PRIVATE ${DAUKLE_WARNING_FLAGS})
target_link_libraries(daukle_test_support PUBLIC daukle_vendor)

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
    # ctest's own default is 1500s, long enough that a reintroduced sequential
    # drain would look like a hung run rather than the deadlock its test catches.
    set_tests_properties(${test_name} PROPERTIES
                         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                         TIMEOUT 120)
endforeach()

# Only on macOS: the other two runners have nothing to check and a skip they
# cannot explain is worse than an absent test.
if(APPLE)
    add_test(NAME macos_links_are_system COMMAND ${CMAKE_COMMAND}
             -DDAUKLE=$<TARGET_FILE:daukle>
             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_macos_links.cmake)
endif()

add_test(NAME agnostic_core COMMAND ${CMAKE_COMMAND}
         -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
         -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_agnostic.cmake)
