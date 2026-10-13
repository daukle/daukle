#ifndef DAUKLE_ARCHIVE_INTERNAL_H
#define DAUKLE_ARCHIVE_INTERNAL_H

#include "archive/archive.h"

#include "miniz.h"

#include <stddef.h>
#include <stdio.h>

/* The seam between archive.c, which detects and opens an archive, and the files
   that read its formats: the byte stream and gzip header, tar, and zip. The
   reader state and every format constant they share live here. */

#define GZIP_FIXED_HEADER_SIZE 10u
#define GZIP_FLAG_HEADER_CRC 0x02u
#define GZIP_FLAG_EXTRA 0x04u
#define GZIP_FLAG_NAME 0x08u
#define GZIP_FLAG_COMMENT 0x10u
#define GZIP_METHOD_DEFLATE 8u

#define ZIP_LOCAL_HEADER 0x04034b50u
#define ZIP_LOCAL_HEADER_SIZE 30u
#define ZIP_END_OF_CENTRAL_DIRECTORY 0x06054b50u
#define ZIP_END_OF_CENTRAL_DIRECTORY_SIZE 22u
#define ZIP_MAX_COMMENT 65535u
#define ZIP_FLAG_DATA_DESCRIPTOR 0x0008u
#define ZIP_SIZE_IN_ZIP64_EXTRA 0xffffffffu
#define ZIP_MADE_ON_UNIX 3u

#define INFLATE_INPUT_BYTES 4096
#define ARCHIVE_FIRST_MEMBER_BYTES 65536u

typedef struct {
    tinfl_decompressor inflator;
    unsigned char window[TINFL_LZ_DICT_SIZE];
    size_t ready_start;
    size_t ready_count;
    unsigned char input[INFLATE_INPUT_BYTES];
    size_t input_offset;
    size_t input_count;
    int input_at_end;
    int finished;
} inflate_stream;

struct fr_archive {
    fr_archive_kind kind;
    FILE *file;
    long length;
    size_t max_member_bytes;
    char *member_bytes;
    size_t member_capacity;

    inflate_stream *stream;
    size_t offset;

    mz_zip_archive zip;
    int zip_is_open;
    mz_uint zip_index;

    char name[FR_ARCHIVE_MAX_NAME + 1];
    char link_target[101];
    /* Set by a pax extended header and consumed by the member that follows it,
       which is the whole of what "extended" means: the records describe the
       NEXT member and nothing else. */
    char pax_name[FR_ARCHIVE_MAX_NAME + 1];
    char pax_link_target[101];
    fr_archive_member member;
    int done;
};

int fr_archive_read_exactly(fr_archive *archive, unsigned char *into, size_t count);
int fr_archive_skip_exactly(fr_archive *archive, size_t count);
int fr_archive_refuse_past_cap(fr_archive *archive, unsigned long long size, fr_error *err);
int fr_archive_fits_the_member_buffer(fr_archive *archive, size_t length, fr_error *err);
void fr_archive_end_iteration(fr_archive *archive, const fr_archive_member **out_member);
void fr_archive_publish(fr_archive *archive, fr_member_kind kind, size_t length, int executable,
                        int setuid, const fr_archive_member **out_member);
int fr_archive_tar_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err);

unsigned long fr_archive_read_little_endian(const unsigned char *bytes, size_t width);
int fr_archive_read_at(FILE *file, unsigned long long offset, unsigned char *into, size_t count);
int fr_archive_zip_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err);

#endif
