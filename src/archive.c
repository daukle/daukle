#include "archive.h"

#include "error.h"
#include "tar.h"

#include "miniz.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    inflate_stream *stream;
    size_t offset;

    mz_zip_archive zip;
    int zip_is_open;
    mz_uint zip_index;

    char name[FR_ARCHIVE_MAX_NAME + 1];
    char link_target[101];
    fr_archive_member member;
    int done;
};

static unsigned long read_little_endian(const unsigned char *bytes, size_t width) {
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
        if (read_little_endian(bytes + scan, 4) == ZIP_END_OF_CENTRAL_DIRECTORY) return 1;
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

static int read_at(FILE *file, unsigned long long offset, unsigned char *into, size_t count) {
    if (offset > (unsigned long long) LONG_MAX) return 0;
    if (fseek(file, (long) offset, SEEK_SET) != 0) return 0;
    return fread(into, 1, count, file) == count;
}

static size_t zip_read(void *opaque, mz_uint64 file_offset, void *into, size_t count) {
    fr_archive *archive = opaque;
    if (!read_at(archive->file, (unsigned long long) file_offset, into, count)) return 0;
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

static int read_exactly(fr_archive *archive, unsigned char *into, size_t count) {
    size_t filled = archive->stream != NULL ? inflate_read(archive, into, count)
                                            : fread(into, 1, count, archive->file);
    return filled == count;
}

static int skip_exactly(fr_archive *archive, size_t count) {
    unsigned char scratch[FR_TAR_BLOCK];
    while (count > 0) {
        size_t take = count < sizeof scratch ? count : sizeof scratch;
        if (!read_exactly(archive, scratch, take)) return 0;
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
    return fseek(file, (long) read_little_endian(length, 2), SEEK_CUR) == 0;
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

static int refuse_past_cap(fr_archive *archive, unsigned long long size, fr_error *err) {
    if (size > (unsigned long long) archive->max_member_bytes) {
        fr_error_set(err, "the archive member \"%s\" is larger than the %zu byte cap",
                     archive->name, archive->max_member_bytes);
        return 1;
    }
    return 0;
}

/* The bound on the write itself, which is not allowed to depend on the cap
   having been applied first. */
static int fits_the_member_buffer(fr_archive *archive, size_t length, fr_error *err) {
    if (length > archive->max_member_bytes) {
        fr_error_set(err, "the archive member \"%s\" does not fit the buffer it is read into",
                     archive->name);
        return 0;
    }
    return 1;
}

static void end_iteration(fr_archive *archive, const fr_archive_member **out_member) {
    archive->done = 1;
    *out_member = NULL;
}

static void publish(fr_archive *archive, fr_member_kind kind, size_t length, int executable,
                    const fr_archive_member **out_member) {
    archive->member.name = archive->name;
    archive->member.kind = kind;
    archive->member.bytes = kind == FR_MEMBER_FILE ? archive->member_bytes : NULL;
    archive->member.length = kind == FR_MEMBER_FILE ? length : 0;
    archive->member.link_target = kind == FR_MEMBER_SYMLINK ? archive->link_target : NULL;
    archive->member.executable = executable;
    *out_member = &archive->member;
}

static int tar_member_kind(char typeflag, fr_member_kind *out) {
    if (typeflag == '0' || typeflag == '\0') {
        *out = FR_MEMBER_FILE;
        return 1;
    }
    if (typeflag == '5') {
        *out = FR_MEMBER_DIRECTORY;
        return 1;
    }
    if (typeflag == '2') {
        *out = FR_MEMBER_SYMLINK;
        return 1;
    }
    return 0;
}

static int tar_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    unsigned char block[FR_TAR_BLOCK];
    if (!read_exactly(archive, block, sizeof block)) {
        fr_error_set(err, "the archive ends before a member header at offset %zu", archive->offset);
        return FR_ERR;
    }

    fr_tar_header header;
    int end_of_archive = 0;
    if (fr_tar_read_header(block, archive->offset, &header, &end_of_archive, err) != FR_OK) {
        return FR_ERR;
    }
    archive->offset += FR_TAR_BLOCK;
    if (end_of_archive) {
        end_iteration(archive, out_member);
        return FR_OK;
    }

    memcpy(archive->name, header.name, strlen(header.name) + 1);
    memcpy(archive->link_target, header.link_target, sizeof archive->link_target);

    fr_member_kind kind;
    if (!tar_member_kind(header.typeflag, &kind)) {
        fr_error_set(err, "the archive member \"%s\" is a type daukle does not unpack (type '%c')",
                     archive->name, header.typeflag);
        return FR_ERR;
    }
    if (refuse_past_cap(archive, header.size, err)) return FR_ERR;

    size_t length = (size_t) header.size;
    if (!fits_the_member_buffer(archive, length, err)) return FR_ERR;
    if (length > 0 && !read_exactly(archive, (unsigned char *) archive->member_bytes, length)) {
        fr_error_set(err, "the archive ends inside \"%s\"", archive->name);
        return FR_ERR;
    }
    archive->member_bytes[length] = '\0';

    size_t padding = (FR_TAR_BLOCK - (length % FR_TAR_BLOCK)) % FR_TAR_BLOCK;
    if (!skip_exactly(archive, padding)) {
        fr_error_set(err, "the archive ends inside \"%s\"", archive->name);
        return FR_ERR;
    }
    archive->offset += length + padding;

    publish(archive, kind, length, (header.mode & 0111u) != 0, out_member);
    return FR_OK;
}

/* A local header and the central directory that disagree are two readers'
   worth of truth about one member, which is the whole parsing differential. */
static int local_header_agrees(fr_archive *archive, const mz_zip_archive_file_stat *entry,
                               fr_error *err) {
    unsigned char header[ZIP_LOCAL_HEADER_SIZE];
    if (!read_at(archive->file, entry->m_local_header_ofs, header, sizeof header)
        || read_little_endian(header, 4) != ZIP_LOCAL_HEADER) {
        fr_error_set(err, "the archive member \"%s\" has no local header", archive->name);
        return 0;
    }

    size_t name_length = (size_t) read_little_endian(header + 26, 2);
    char local_name[FR_ARCHIVE_MAX_NAME + 1];
    if (name_length > FR_ARCHIVE_MAX_NAME
        || !read_at(archive->file, entry->m_local_header_ofs + ZIP_LOCAL_HEADER_SIZE,
                    (unsigned char *) local_name, name_length)) {
        fr_error_set(err, "the archive member \"%s\" has an unreadable local header",
                     archive->name);
        return 0;
    }
    local_name[name_length] = '\0';
    if (strcmp(local_name, archive->name) != 0) {
        fr_error_set(err, "the archive member \"%s\" calls itself \"%s\" in its local header",
                     archive->name, local_name);
        return 0;
    }

    unsigned long flags = read_little_endian(header + 6, 2);
    unsigned long local_size = read_little_endian(header + 22, 4);
    if (local_size == ZIP_SIZE_IN_ZIP64_EXTRA) return 1;
    /* Bit 3 moves the sizes to a trailing descriptor and zeroes them here. */
    if ((flags & ZIP_FLAG_DATA_DESCRIPTOR) != 0 && local_size == 0) return 1;

    if ((unsigned long long) local_size != entry->m_uncomp_size) {
        fr_error_set(err,
                     "the archive member \"%s\" is %lu bytes by its local header and %llu by the"
                     " directory",
                     archive->name, local_size, (unsigned long long) entry->m_uncomp_size);
        return 0;
    }
    return 1;
}

static int zip_executable(const mz_zip_archive_file_stat *entry) {
    if ((entry->m_version_made_by >> 8) != ZIP_MADE_ON_UNIX) return 0;
    return ((entry->m_external_attr >> 16) & 0111u) != 0;
}

static int zip_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    if (archive->zip_index >= mz_zip_reader_get_num_files(&archive->zip)) {
        end_iteration(archive, out_member);
        return FR_OK;
    }

    mz_zip_archive_file_stat entry;
    if (!mz_zip_reader_file_stat(&archive->zip, archive->zip_index, &entry)) {
        fr_error_set(err, "the archive has an unreadable directory entry at index %u",
                     (unsigned) archive->zip_index);
        return FR_ERR;
    }

    size_t name_length = strlen(entry.m_filename);
    if (name_length > FR_ARCHIVE_MAX_NAME) {
        fr_error_set(err, "the archive has a member with a name longer than %d bytes",
                     FR_ARCHIVE_MAX_NAME);
        return FR_ERR;
    }
    memcpy(archive->name, entry.m_filename, name_length + 1);

    if (!local_header_agrees(archive, &entry, err)) return FR_ERR;

    archive->zip_index++;
    if (name_length > 0 && archive->name[name_length - 1] == '/') {
        publish(archive, FR_MEMBER_DIRECTORY, 0, zip_executable(&entry), out_member);
        return FR_OK;
    }

    if (refuse_past_cap(archive, entry.m_uncomp_size, err)) return FR_ERR;

    size_t length = (size_t) entry.m_uncomp_size;
    if (!fits_the_member_buffer(archive, length, err)) return FR_ERR;
    if (length > 0
        && !mz_zip_reader_extract_to_mem(&archive->zip, archive->zip_index - 1,
                                         archive->member_bytes, length, 0)) {
        fr_error_set(err, "the archive member \"%s\" cannot be decompressed (%s)", archive->name,
                     mz_zip_get_error_string(mz_zip_get_last_error(&archive->zip)));
        return FR_ERR;
    }
    archive->member_bytes[length] = '\0';

    publish(archive, FR_MEMBER_FILE, length, zip_executable(&entry), out_member);
    return FR_OK;
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

static int decide_kind(fr_archive *archive, fr_error *err) {
    unsigned char head[FR_TAR_BLOCK];
    size_t got = fread(head, 1, sizeof head, archive->file);
    rewind(archive->file);

    if (looks_like_gzip(head, got)) archive->kind = FR_ARCHIVE_TAR_GZ;
    else if (fr_tar_looks_like_archive((const char *) head, got)) archive->kind = FR_ARCHIVE_TAR;
    else if (looks_like_zip_start(head, got)) archive->kind = FR_ARCHIVE_ZIP;
    else {
        fr_error_set(err, "the archive is not a tar, a tar.gz or a zip");
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
    archive->member_bytes = malloc(max_member_bytes + 1);
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

    if (measure(archive, err) != FR_OK || decide_kind(archive, err) != FR_OK
        || start_reading(archive, err) != FR_OK) {
        fr_archive_close(archive);
        return FR_ERR;
    }

    *out = archive;
    return FR_OK;
}

int fr_archive_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    *out_member = NULL;
    if (archive->done) return FR_OK;
    if (archive->kind == FR_ARCHIVE_ZIP) return zip_next(archive, out_member, err);
    return tar_next(archive, out_member, err);
}

void fr_archive_close(fr_archive *archive) {
    if (archive == NULL) return;
    if (archive->zip_is_open) mz_zip_reader_end(&archive->zip);
    if (archive->file != NULL) fclose(archive->file);
    free(archive->stream);
    free(archive->member_bytes);
    free(archive);
}
