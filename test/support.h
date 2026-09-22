#ifndef DAUKLE_TEST_SUPPORT_H
#define DAUKLE_TEST_SUPPORT_H

#include <stddef.h>

const char *fr_test_temp_base(void);
int fr_test_process_id(void);
int fr_test_make_directory(const char *path);
void fr_test_remove_tree(const char *path);
int fr_test_count_files(const char *root, const char *file_name);
void fr_test_set_env(const char *name, const char *value);
int fr_test_get_working_directory(char *buffer, size_t size);
int fr_test_set_working_directory(const char *path);
void fr_test_prepend_to_path_dir_of(const char *argv_zero);

/* Seconds since the epoch, or 0 when the file cannot be stat'd, so a test can
   assert that an unchanged file was not rewritten. Content equality cannot:
   an implementation that rewrites an identical file every run passes it. */
long long fr_test_file_mtime(const char *path);

/* Sleeps just past the coarsest modification-time resolution the suite runs on,
   so "the mtime did not change" means the write did not happen rather than that
   the two writes landed in one tick. */
void fr_test_sleep_past_mtime_resolution(void);

#endif
