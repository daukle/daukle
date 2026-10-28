#ifndef DAUKLE_WRAPPER_PIN_H
#define DAUKLE_WRAPPER_PIN_H

#include <stddef.h>

/* The quoted value of the top-level `version = "..."` in a pin, which may be empty. Returns 0 when
   no such line comes before the first `[` section or the value does not fit in size.
   @implNote Line-based on purpose, as the wrapper scripts read the same key: a pin is three keys
   and per-host sections, not arbitrary TOML. */
int fr_wrapper_pin_version(const char *text, char *out, size_t size);

/* The pin a distribution ships, looked for in order: override (DAUKLE_WRAPPER_PIN) when it is
   non-empty, then wrapper.toml in executable_directory, then ../share/daukle/wrapper.toml relative
   to it. Returns 1 with *out_found set to the first that exists, or 0 with *out_searched set to
   every location looked at, one per line. Either string is the caller's to free. A NULL
   executable_directory searches the override alone. */
int fr_wrapper_pin_find(const char *override, const char *executable_directory, char **out_found,
                        char **out_searched);

#endif
