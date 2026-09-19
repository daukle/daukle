#include "greatest.h"
#include "luax.h"
#include "cJSON.h"

#include <string.h>

TEST opens_and_runs_a_chunk(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT(state != NULL);
    ASSERT_EQ(FR_OK, fr_lua_run(state, "local x = 1 + 1", "=test", &err));
    fr_lua_close(state);
    PASS();
}

TEST reports_a_syntax_error_with_its_chunk_name(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "local = =", "daukle.lua", &err));
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST reports_a_runtime_error_with_its_line(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "\nerror('boom')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "boom") != NULL);
    ASSERT(strstr(err.message, "daukle.lua:2") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST stops_a_script_that_allocates_without_end(void) {
    fr_error err;
    lua_State *state = fr_lua_open(1u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state,
        "local t = {} local i = 1 while true do t[i] = string.rep('x', 1024) i = i + 1 end",
        "daukle.lua", &err));
    ASSERT(strstr(err.message, "not enough memory") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST fails_gracefully_when_the_cap_is_too_tight_for_the_standard_libraries(void) {
    fr_error err;
    /* Big enough for lua_newstate to construct the state, too small for
       luaL_openlibs to finish; empirically bracketed between 5120 (state built,
       libs fail) and 19456 (still failing) below 20480 (libs succeed). Before the
       protected-pcall fix in fr_lua_open, this window aborted the process instead
       of returning NULL. */
    lua_State *state = fr_lua_open(8u * 1024u, &err);
    ASSERT(state == NULL);
    ASSERT(strstr(err.message, "not enough memory") != NULL);
    PASS();
}

TEST pushes_a_json_document_as_a_lua_table(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    cJSON *document = cJSON_Parse(
        "{\"name\":\"x\",\"count\":2,\"on\":true,\"off\":false,\"none\":null,"
        "\"list\":[\"a\",\"b\"],\"nested\":{\"key\":\"value\"}}");
    ASSERT(document != NULL);

    int stack_before = lua_gettop(state);
    ASSERT_EQ(FR_OK, fr_lua_push_json(state, document, &err));
    ASSERT_EQ(stack_before + 1, lua_gettop(state));
    lua_setglobal(state, "document");
    cJSON_Delete(document);

    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "assert(document.name == 'x')\n"
        "assert(document.count == 2)\n"
        "assert(document.on == true)\n"
        "assert(document.off == false)\n"
        "assert(document.none == nil)\n"
        "assert(#document.list == 2 and document.list[2] == 'b')\n"
        "assert(document.nested.key == 'value')\n", "=check", &err));
    fr_lua_close(state);
    PASS();
}

TEST reads_a_lua_table_back_as_json(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "result = { name = 'x', count = 2, on = true, list = { 'a', 'b' },"
        " nested = { key = 'value' }, empty = {} }", "=build", &err));
    lua_getglobal(state, "result");

    int stack_before = lua_gettop(state);
    cJSON *document = NULL;
    ASSERT_EQ(FR_OK, fr_lua_to_json(state, -1, &document, &err));
    ASSERT_EQ(stack_before, lua_gettop(state));
    ASSERT_STR_EQ("x", cJSON_GetObjectItemCaseSensitive(document, "name")->valuestring);
    ASSERT_EQ(2, cJSON_GetObjectItemCaseSensitive(document, "count")->valueint);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(document, "on")));
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(document, "list")));
    ASSERT_EQ(2, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(document, "list")));
    ASSERT_STR_EQ("value",
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(document, "nested"), "key")->valuestring);
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(document, "empty")));

    cJSON_Delete(document);
    fr_lua_close(state);
    PASS();
}

TEST rejects_a_table_key_that_is_not_a_string(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    fr_lua_run(state, "result = { [true] = 'x' }", "=build", &err);
    lua_getglobal(state, "result");
    int stack_before = lua_gettop(state);
    cJSON *document = NULL;
    ASSERT_EQ(FR_ERR, fr_lua_to_json(state, -1, &document, &err));
    ASSERT_EQ(stack_before, lua_gettop(state));
    ASSERT_EQ(NULL, document);
    fr_lua_close(state);
    PASS();
}

TEST reads_a_lua_array_back_in_written_order(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    /* Padding keys force 1..6 into the hash part, so lua_next visits them out of order. */
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "result = {}\n"
        "for i = 1, 20 do result['padding' .. i] = i end\n"
        "result[1] = 'first' result[2] = 'second' result[3] = 'third'\n"
        "result[4] = 'fourth' result[5] = 'fifth' result[6] = 'sixth'\n"
        "for i = 1, 20 do result['padding' .. i] = nil end",
        "=build", &err));
    lua_getglobal(state, "result");

    cJSON *document = NULL;
    ASSERT_EQ(FR_OK, fr_lua_to_json(state, -1, &document, &err));
    ASSERT(cJSON_IsArray(document));
    ASSERT_EQ(6, cJSON_GetArraySize(document));
    ASSERT_STR_EQ("first", cJSON_GetArrayItem(document, 0)->valuestring);
    ASSERT_STR_EQ("second", cJSON_GetArrayItem(document, 1)->valuestring);
    ASSERT_STR_EQ("third", cJSON_GetArrayItem(document, 2)->valuestring);
    ASSERT_STR_EQ("fourth", cJSON_GetArrayItem(document, 3)->valuestring);
    ASSERT_STR_EQ("fifth", cJSON_GetArrayItem(document, 4)->valuestring);
    ASSERT_STR_EQ("sixth", cJSON_GetArrayItem(document, 5)->valuestring);

    cJSON_Delete(document);
    fr_lua_close(state);
    PASS();
}

TEST refuses_a_lua_table_nested_past_the_depth_cap(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "result = {}\n"
        "local node = result\n"
        "for i = 1, 200 do node.child = {} node = node.child end",
        "=build", &err));
    lua_getglobal(state, "result");
    int stack_before = lua_gettop(state);

    cJSON *document = NULL;
    ASSERT_EQ(FR_ERR, fr_lua_to_json(state, -1, &document, &err));
    ASSERT_EQ(NULL, document);
    ASSERT_EQ(stack_before, lua_gettop(state));
    ASSERT(strstr(err.message, "nests deeper") != NULL);

    fr_lua_close(state);
    PASS();
}

/* "daukle.config.self = daukle.config" is one line to write and would otherwise
   recurse until the C stack died, where the author expects an error. */
TEST refuses_a_table_that_refers_to_itself(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_OK, fr_lua_run(state, "result = {} result.self = result", "=build", &err));
    lua_getglobal(state, "result");
    int stack_before = lua_gettop(state);

    cJSON *document = NULL;
    ASSERT_EQ(FR_ERR, fr_lua_to_json(state, -1, &document, &err));
    ASSERT_EQ(NULL, document);
    ASSERT_EQ(stack_before, lua_gettop(state));

    fr_lua_close(state);
    PASS();
}

TEST refuses_a_json_document_nested_past_the_depth_cap(void) {
    char deep[512];
    size_t nesting = 200;
    for (size_t index = 0; index < nesting; index++) deep[index] = '[';
    for (size_t index = 0; index < nesting; index++) deep[nesting + index] = ']';
    deep[nesting * 2] = '\0';

    cJSON *document = cJSON_Parse(deep);
    ASSERT(document != NULL);

    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    int stack_before = lua_gettop(state);
    ASSERT_EQ(FR_ERR, fr_lua_push_json(state, document, &err));
    ASSERT_EQ(stack_before, lua_gettop(state));
    ASSERT(strstr(err.message, "nests deeper") != NULL);

    fr_lua_close(state);
    cJSON_Delete(document);
    PASS();
}

TEST reports_an_error_object_that_is_not_a_string(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "error({})", "=build", &err));
    ASSERT(err.message[0] != '\0');
    fr_lua_close(state);
    PASS();
}

TEST runs_a_chunk_under_a_supplied_environment(void) {
    fr_error err;
    lua_State *state = fr_lua_open(8u * 1024u * 1024u, &err);
    ASSERT(state != NULL);

    lua_newtable(state);
    lua_pushinteger(state, 7);
    lua_setfield(state, -2, "seed");
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state, "result = seed * 3", "=test", env, &err));

    lua_getfield(state, env, "result");
    ASSERT_EQ(21, (int) lua_tointeger(state, -1));
    lua_pop(state, 1);

    lua_getglobal(state, "result");
    ASSERT(lua_isnil(state, -1));
    lua_pop(state, 1);

    ASSERT_EQ(env, lua_gettop(state));
    fr_lua_close(state);
    PASS();
}

TEST reports_an_error_from_a_chunk_run_under_an_environment(void) {
    fr_error err;
    lua_State *state = fr_lua_open(8u * 1024u * 1024u, &err);
    ASSERT(state != NULL);

    lua_newtable(state);
    lua_getglobal(state, "error");
    lua_setfield(state, -2, "error");
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state, "error('boom')", "=test", env, &err));
    ASSERT(strstr(err.message, "boom") != NULL);
    ASSERT_EQ(env, lua_gettop(state));

    fr_lua_close(state);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(opens_and_runs_a_chunk);
    RUN_TEST(reports_a_syntax_error_with_its_chunk_name);
    RUN_TEST(reports_a_runtime_error_with_its_line);
    RUN_TEST(stops_a_script_that_allocates_without_end);
    RUN_TEST(fails_gracefully_when_the_cap_is_too_tight_for_the_standard_libraries);
    RUN_TEST(pushes_a_json_document_as_a_lua_table);
    RUN_TEST(reads_a_lua_table_back_as_json);
    RUN_TEST(rejects_a_table_key_that_is_not_a_string);
    RUN_TEST(reads_a_lua_array_back_in_written_order);
    RUN_TEST(refuses_a_lua_table_nested_past_the_depth_cap);
    RUN_TEST(refuses_a_table_that_refers_to_itself);
    RUN_TEST(refuses_a_json_document_nested_past_the_depth_cap);
    RUN_TEST(reports_an_error_object_that_is_not_a_string);
    RUN_TEST(runs_a_chunk_under_a_supplied_environment);
    RUN_TEST(reports_an_error_from_a_chunk_run_under_an_environment);
    GREATEST_MAIN_END();
}
