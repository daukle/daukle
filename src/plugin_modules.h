#ifndef DAUKLE_PLUGIN_MODULES_H
#define DAUKLE_PLUGIN_MODULES_H

#include "types.h"

#include "lua.h"

#include <stddef.h>

/* Where a plugin's code comes from, with one interface over the three forms it
   can take: one lua chunk, a tar archive held in memory, and a local directory
   read as it is asked for. Everything above this sees "give me member X" and
   knows which of the three it is talking to only through the errors it gets. */
typedef struct fr_plugin_source fr_plugin_source;

/* Decides by reading whether bytes are an archive or a single chunk. bytes must
   outlive the source: an archive member points into them rather than copying.
   An archive is validated here rather than in tar.c, because "every member is
   lua" and "no member name climbs out" are daukle's rules and tar.c is not
   allowed to know daukle. */
int fr_plugin_source_open_bytes(const char *bytes, size_t length, fr_plugin_source **out,
                                fr_error *err);

/* A directory holding plugin.lua, named as the manifest wrote it, relative to
   the sandbox base. Members resolve through fr_lua_sandbox_resolve as
   daukle.include does, so a symlink inside the plugin cannot reach out of the
   project, and neither component can carry a "..". */
int fr_plugin_source_open_directory(lua_State *state, const char *relative_directory,
                                    fr_plugin_source **out, fr_error *err);

/* The bytes that run: the whole chunk, or the member named plugin.lua. Borrowed
   from the source and valid until it is closed. */
int fr_plugin_source_entry(fr_plugin_source *source, const char **out_text, size_t *out_length,
                           fr_error *err);

/* module_name is a module name and not a file name: ".lua" is appended here and
   may not be written by the caller, so one module has one spelling. */
int fr_plugin_source_member(fr_plugin_source *source, const char *module_name,
                            const char **out_text, size_t *out_length, fr_error *err);

void fr_plugin_source_close(fr_plugin_source *source);

#endif
