#include "archive/archive.h"
#include "archive/archive_internal.h"

#include "archive/tar.h"
#include "util/error.h"

#include "miniz.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long fr_archive_read_little_endian(const unsigned char *bytes, size_t width) {
    unsigned long value = 0;
    for (size_t index = width; index > 0; index--) {
        value = (value << 8) | bytes[index - 1];
    }
    return value;
}

static int looks_like_gzip(const unsigned char *bytes, size_t length) {
    return length >= 2 && bytes[0] == 0x1fu && bytes[1] == 0x8bu;
}

/* fr_archive_open knows the head of a file it can seek in, and miniz does the
   real end-of-directory scan when the reader starts, so the head signature is
   all that has to be decided here. */
static int looks_like_zip_start(const unsigned char *bytes, size_t length) {
    return length >= 4 && bytes[0] == 'P' && bytes[1] == 'K'
           && ((bytes[2] == 0x03u && bytes[3] == 0x04u)
               || (bytes[2] == 0x05u && bytes[3] == 0x06u));
}

static int has_end_of_central_directory(const unsigned char *bytes, size_t length) {
    if (length < ZIP_END_OF_CENTRAL_DIRECTORY_SIZE) return 0;

    size_t scan = length - ZIP_END_OF_CENTRAL_DIRECTORY_SIZE;
    size_t stop = 0;
    if (length > ZIP_MAX_COMMENT + ZIP_END_OF_CENTRAL_DIRECTORY_SIZE) {
        stop = length - ZIP_MAX_COMMENT - ZIP_END_OF_CENTRAL_DIRECTORY_SIZE;
    }
    for (;;) {
        if (fr_archive_read_little_endian(bytes + scan, 4) == ZIP_END_OF_CENTRAL_DIRECTORY) return 1;
        if (scan == stop) return 0;
        scan--;
    }
}

int fr_archive_kind_of(const char *bytes, size_t length, fr_archive_kind *out, fr_error *err) {
    const unsigned char *raw = (const unsigned char *) bytes;
    if (bytes != NULL && looks_like_gzip(raw, length)) {
        *out = FR_ARCHIVE_TAR_GZ;
        return FR_OK;
    }
    if (fr_tar_looks_like_archive(bytes, length)) {
        *out = FR_ARCHIVE_TAR;
        return FR_OK;
    }
    if (bytes != NULL && has_end_of_central_directory(raw, length)) {
        *out = FR_ARCHIVE_ZIP;
        return FR_OK;
    }
    fr_error_set(err, "the bytes are not a tar, a tar.gz or a zip");
    return FR_ERR;
}

int fr_archive_read_at(FILE *file, unsigned long long offset, unsigned char *into, size_t count) {
    if (offset > (unsigned long long) LONG_MAX) return 0;
    if (fseek(file, (long) offset, SEEK_SET) != 0) return 0;
    return fread(into, 1, count, file) == count;
}

static size_t zip_read(void *opaque, mz_uint64 file_offset, void *into, size_t count) {
    fr_archive *archive = opaque;
    if (!fr_archive_read_at(archive->file, (unsigned long long) file_offset, into, count)) return 0;
    return count;
}

static int pump(inflate_stream *stream, FILE *file) {
    while (stream->ready_count == 0 && !stream->finished) {
        if (stream->input_offset >= stream->input_count) {
            stream->input_offset = 0;
            stream->input_count = fread(stream->input, 1, sizeof stream->input, file);
            if (stream->input_count == 0) stream->input_at_end = 1;
        }

        size_t consumed = stream->input_count - stream->input_offset;
        size_t produced = TINFL_LZ_DICT_SIZE - stream->ready_start;
        tinfl_status status =
            tinfl_decompress(&stream->inflator, stream->input + stream->input_offset, &consumed,
                             stream->window, stream->window + stream->ready_start, &produced,
                             stream->input_at_end ? 0u : (mz_uint32) TINFL_FLAG_HAS_MORE_INPUT);
        stream->input_offset += consumed;
        stream->ready_count = produced;
        if (status < TINFL_STATUS_DONE) return 0;
        if (status == TINFL_STATUS_DONE) stream->finished = 1;
    }
    return stream->ready_count > 0;
}

/* The window is both the output and the back-reference dictionary, so bytes are
   handed out from it rather than copied out of a separate buffer. */
static size_t inflate_read(fr_archive *archive, unsigned char *into, size_t count) {
    inflate_stream *stream = archive->stream;
    size_t filled = 0;
    while (filled < count) {
        if (stream->ready_count == 0 && !pump(stream, archive->file)) break;

        size_t take = count - filled;
        if (take > stream->ready_count) take = stream->ready_count;
        memcpy(into + filled, stream->window + stream->ready_start, take);
        stream->ready_start = (stream->ready_start + take) & (TINFL_LZ_DICT_SIZE - 1);
        stream->ready_count -= take;
        filled += take;
    }
    return filled;
}

int fr_archive_read_exactly(fr_archive *archive, unsigned char *into, size_t count) {
    size_t filled = archive->stream != NULL ? inflate_read(archive, into, count)
                                            : fread(into, 1, count, archive->file);
    return filled == count;
}

int fr_archive_skip_exactly(fr_archive *archive, size_t count) {
    unsigned char scratch[FR_TAR_BLOCK];
    while (count > 0) {
        size_t take = count < sizeof scratch ? count : sizeof scratch;
        if (!fr_archive_read_exactly(archive, scratch, take)) return 0;
        count -= take;
    }
    return 1;
}

static int skip_zero_terminated(FILE *file) {
    for (;;) {
        int byte = fgetc(file);
        if (byte == EOF) return 0;
        if (byte == 0) return 1;
    }
}

static int truncated_gzip_header(fr_error *err) {
    fr_error_set(err, "the archive ends inside its gzip header");
    return FR_ERR;
}

static int skip_gzip_extra_field(FILE *file) {
    unsigned char length[2];
    if (fread(length, 1, sizeof length, file) != sizeof length) return 0;
    return fseek(file, (long) fr_archive_read_little_endian(length, 2), SEEK_CUR) == 0;
}

/* gzip's header is variable: gzip(1) writes the original file name after the
   ten fixed bytes, which miniz's own writer never does. */
static int skip_gzip_header(FILE *file, fr_error *err) {
    unsigned char fixed[GZIP_FIXED_HEADER_SIZE];
    if (fread(fixed, 1, sizeof fixed, file) != sizeof fixed) return truncated_gzip_header(err);
    if (fixed[2] != GZIP_METHOD_DEFLATE) {
        fr_error_set(err, "the archive is gzipped with method %u rather than deflate", fixed[2]);
        return FR_ERR;
    }

    unsigned flags = fixed[3];
    if ((flags & GZIP_FLAG_EXTRA) != 0 && !skip_gzip_extra_field(file)) {
        return truncated_gzip_header(err);
    }
    if ((flags & GZIP_FLAG_NAME) != 0 && !skip_zero_terminated(file)) {
        return truncated_gzip_header(err);
    }
    if ((flags & GZIP_FLAG_COMMENT) != 0 && !skip_zero_terminated(file)) {
        return truncated_gzip_header(err);
    }
    if ((flags & GZIP_FLAG_HEADER_CRC) != 0 && fseek(file, 2, SEEK_CUR) != 0) {
        return truncated_gzip_header(err);
    }
    return FR_OK;
}

int fr_archive_refuse_past_cap(fr_archive *archive, unsigned long long size, fr_error *err) {
    if (size > (unsigned long long) archive->max_member_bytes) {
        fr_error_set(err, "the archive member \"%s\" is larger than the %zu byte cap",
                     archive->name, archive->max_member_bytes);
        return 1;
    }
    return 0;
}

/* The bound on the write itself, which is not allowed to depend on the cap
   having been applied first.
   @implNote The buffer grows to the largest member seen rather than to the cap,
   because committing max_member_bytes up front charges a 2 KB tarball the full
   256 MiB and a memory-capped container then cannot read an archive that
   trivially fits. The cap still bounds the growth. */
int fr_archive_fits_the_member_buffer(fr_archive *archive, size_t length, fr_error *err) {
    if (length > archive->max_member_bytes) {
        fr_error_set(err, "the archive member \"%s\" does not fit the buffer it is read into",
                     archive->name);
        return 0;
    }
    if (length + 1 <= archive->member_capacity) return 1;

    size_t capacity = archive->member_capacity;
    while (capacity < length + 1) {
        if (capacity > (size_t) -1 / 2) {
            capacity = length + 1;
            break;
        }
        capacity *= 2;
    }
    if (capacity > archive->max_member_bytes && archive->max_member_bytes < (size_t) -1) {
        capacity = archive->max_member_bytes + 1;
    }

    char *grown = realloc(archive->member_bytes, capacity);
    if (grown == NULL) {
        fr_error_set(err, "there is not enough memory to read the archive member \"%s\"",
                     archive->name);
        return 0;
    }
    archive->member_bytes = grown;
    archive->member_capacity = capacity;
    return 1;
}

void fr_archive_end_iteration(fr_archive *archive, const fr_archive_member **out_member) {
    archive->done = 1;
    *out_member = NULL;
}

void fr_archive_publish(fr_archive *archive, fr_member_kind kind, size_t length, int executable,
                        int setuid, const fr_archive_member **out_member) {
    archive->member.name = archive->name;
    archive->member.kind = kind;
    archive->member.bytes = kind == FR_MEMBER_FILE ? archive->member_bytes : NULL;
    archive->member.length = kind == FR_MEMBER_FILE ? length : 0;
    archive->member.payload_length = length;
    archive->member.link_target = kind == FR_MEMBER_SYMLINK ? archive->link_target : NULL;
    archive->member.executable = executable;
    archive->member.setuid = setuid;
    *out_member = &archive->member;
}

static int measure(fr_archive *archive, fr_error *err) {
    if (fseek(archive->file, 0, SEEK_END) != 0) {
        fr_error_set(err, "the archive cannot be measured");
        return FR_ERR;
    }
    archive->length = ftell(archive->file);
    if (archive->length < 0) {
        fr_error_set(err, "the archive is too large to read");
        return FR_ERR;
    }
    rewind(archive->file);
    return FR_OK;
}

/* name reaches here only to be quoted in a refusal: what the file is called is
   never what decides the kind. */
static int decide_kind(fr_archive *archive, const char *name, fr_error *err) {
    unsigned char head[FR_TAR_BLOCK];
    size_t got = fread(head, 1, sizeof head, archive->file);
    rewind(archive->file);

    if (looks_like_gzip(head, got)) archive->kind = FR_ARCHIVE_TAR_GZ;
    else if (fr_tar_looks_like_archive((const char *) head, got)) archive->kind = FR_ARCHIVE_TAR;
    else if (looks_like_zip_start(head, got)) archive->kind = FR_ARCHIVE_ZIP;
    else {
        fr_error_set(err, "the archive %s is not a tar, a tar.gz or a zip", name);
        return FR_ERR;
    }
    return FR_OK;
}

static int start_reading(fr_archive *archive, fr_error *err) {
    if (archive->kind == FR_ARCHIVE_ZIP) {
        archive->zip.m_pRead = zip_read;
        archive->zip.m_pIO_opaque = archive;
        if (!mz_zip_reader_init(&archive->zip, (mz_uint64) archive->length, 0)) {
            fr_error_set(err, "the archive has no readable zip directory (%s)",
                         mz_zip_get_error_string(mz_zip_get_last_error(&archive->zip)));
            return FR_ERR;
        }
        archive->zip_is_open = 1;
        return FR_OK;
    }

    if (archive->kind == FR_ARCHIVE_TAR_GZ) {
        if (skip_gzip_header(archive->file, err) != FR_OK) return FR_ERR;
        archive->stream = calloc(1, sizeof *archive->stream);
        if (archive->stream == NULL) {
            fr_error_set(err, "there is not enough memory to decompress the archive");
            return FR_ERR;
        }
        tinfl_init(&archive->stream->inflator);
    }
    return FR_OK;
}

int fr_archive_open(const char *path, size_t max_member_bytes, fr_archive **out, fr_error *err) {
    *out = NULL;

    fr_archive *archive = calloc(1, sizeof *archive);
    if (archive == NULL) {
        fr_error_set(err, "there is not enough memory to read an archive");
        return FR_ERR;
    }
    archive->max_member_bytes = max_member_bytes;
    archive->member_capacity = ARCHIVE_FIRST_MEMBER_BYTES;
    if (max_member_bytes < (size_t) -1 && max_member_bytes + 1 < archive->member_capacity) {
        archive->member_capacity = max_member_bytes + 1;
    }
    archive->member_bytes = malloc(archive->member_capacity);
    if (archive->member_bytes == NULL) {
        fr_error_set(err, "there is not enough memory to read an archive");
        fr_archive_close(archive);
        return FR_ERR;
    }
    archive->member_bytes[0] = '\0';

    archive->file = fopen(path, "rb");
    if (archive->file == NULL) {
        fr_error_set(err, "the archive %s cannot be opened", path);
        fr_archive_close(archive);
        return FR_ERR;
    }

    if (measure(archive, err) != FR_OK || decide_kind(archive, path, err) != FR_OK
        || start_reading(archive, err) != FR_OK) {
        fr_archive_close(archive);
        return FR_ERR;
    }

    *out = archive;
    return FR_OK;
}

fr_archive_kind fr_archive_opened_kind(const fr_archive *archive) {
    return archive->kind;
}

int fr_archive_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    *out_member = NULL;
    if (archive->done) return FR_OK;
    if (archive->kind == FR_ARCHIVE_ZIP) return fr_archive_zip_next(archive, out_member, err);
    return fr_archive_tar_next(archive, out_member, err);
}

void fr_archive_close(fr_archive *archive) {
    if (archive == NULL) return;
    if (archive->zip_is_open) mz_zip_reader_end(&archive->zip);
    if (archive->file != NULL) fclose(archive->file);
    free(archive->stream);
    free(archive->member_bytes);
    free(archive);
}
