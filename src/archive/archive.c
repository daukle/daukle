#include "archive/archive.h"

#include "archive/tar.h"
#include "util/error.h"

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
   having been applied first.
   @implNote The buffer grows to the largest member seen rather than to the cap,
   because committing max_member_bytes up front charges a 2 KB tarball the full
   256 MiB and a memory-capped container then cannot read an archive that
   trivially fits. The cap still bounds the growth. */
static int fits_the_member_buffer(fr_archive *archive, size_t length, fr_error *err) {
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

static void end_iteration(fr_archive *archive, const fr_archive_member **out_member) {
    archive->done = 1;
    *out_member = NULL;
}

static void publish(fr_archive *archive, fr_member_kind kind, size_t length, int executable,
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

/* A pax extended header's payload is a run of "<length> <keyword>=<value>\n"
   records, where <length> counts its own digits, the space, the keyword, the
   "=", the value and the newline. */
static int pax_record_length(const char *payload, size_t remaining, size_t *out) {
    size_t digits = 0;
    size_t length = 0;
    while (digits < remaining && payload[digits] >= '0' && payload[digits] <= '9') {
        if (length > (size_t) -1 / 10) return 0;
        length = length * 10 + (size_t) (payload[digits] - '0');
        digits++;
    }
    if (digits == 0 || digits >= remaining || payload[digits] != ' ') return 0;
    if (length <= digits + 1 || length > remaining) return 0;
    *out = length;
    return 1;
}

static int copy_pax_value(const char *value, size_t length, char *out, size_t out_size,
                          const char *keyword, fr_error *err) {
    if (length == 0 || length >= out_size) {
        fr_error_set(err, "the archive has a pax \"%s\" of %zu bytes, and the limit is %zu",
                     keyword, length, out_size - 1);
        return FR_ERR;
    }
    memcpy(out, value, length);
    out[length] = '\0';
    return FR_OK;
}

/* @implNote only "path" and "linkpath" are read. The rest of what GNU tar
   writes here is atime, ctime, mtime, uid and gid, none of which daukle's
   unpack consults, and a keyword that is ignored has to be one nothing
   depends on. A GLOBAL header (type 'g') applies to every member that
   follows rather than to one, so a path in one would rename the whole
   archive: that is refused rather than honoured or quietly dropped. */
static int read_pax_records(fr_archive *archive, const char *payload, size_t length,
                            int is_global, fr_error *err) {
    size_t offset = 0;
    while (offset < length) {
        size_t record = 0;
        if (!pax_record_length(payload + offset, length - offset, &record)) {
            fr_error_set(err, "the archive has a pax header record daukle cannot parse");
            return FR_ERR;
        }
        const char *body = payload + offset;
        size_t digits = 0;
        while (body[digits] != ' ') digits++;
        const char *keyword = body + digits + 1;
        size_t keyword_span = record - digits - 1;

        const char *equals = memchr(keyword, '=', keyword_span);
        if (equals == NULL) {
            fr_error_set(err, "the archive has a pax header record with no \"=\"");
            return FR_ERR;
        }
        size_t keyword_length = (size_t) (equals - keyword);
        const char *value = equals + 1;
        /* The record ends with a newline that is not part of the value. */
        size_t value_length = keyword_span - keyword_length - 1;
        if (value_length > 0 && value[value_length - 1] == '\n') value_length--;

        int is_path = keyword_length == 4 && memcmp(keyword, "path", 4) == 0;
        int is_link = keyword_length == 8 && memcmp(keyword, "linkpath", 8) == 0;
        if ((is_path || is_link) && is_global) {
            fr_error_set(err, "the archive has a GLOBAL pax header naming a \"%s\", which would"
                              " rename every member that follows it",
                         is_path ? "path" : "linkpath");
            return FR_ERR;
        }
        if (is_path && copy_pax_value(value, value_length, archive->pax_name,
                                      sizeof archive->pax_name, "path", err) != FR_OK) {
            return FR_ERR;
        }
        if (is_link && copy_pax_value(value, value_length, archive->pax_link_target,
                                      sizeof archive->pax_link_target, "linkpath", err) != FR_OK) {
            return FR_ERR;
        }
        offset += record;
    }
    return FR_OK;
}

/* A pax header is metadata, not a member, so it is read into a local buffer
   rather than through fits_the_member_buffer: that buffer belongs to the
   member being published and reusing it here would overwrite the payload of
   whatever was published last. The cap is its own, and small, because a path
   is the largest thing read out of one. */
static int read_pax_header(fr_archive *archive, const fr_tar_header *header, fr_error *err) {
    if (header->size > FR_ARCHIVE_MAX_PAX_BYTES) {
        fr_error_set(err, "the archive has a pax header of %llu bytes, and the limit is %d",
                     header->size, FR_ARCHIVE_MAX_PAX_BYTES);
        return FR_ERR;
    }

    char payload[FR_ARCHIVE_MAX_PAX_BYTES];
    size_t length = (size_t) header->size;
    if (length > 0 && !read_exactly(archive, (unsigned char *) payload, length)) {
        fr_error_set(err, "the archive ends inside a pax header");
        return FR_ERR;
    }

    size_t padding = (FR_TAR_BLOCK - (length % FR_TAR_BLOCK)) % FR_TAR_BLOCK;
    if (!skip_exactly(archive, padding)) {
        fr_error_set(err, "the archive ends inside a pax header");
        return FR_ERR;
    }
    archive->offset += length + padding;

    return read_pax_records(archive, payload, length, header->typeflag == 'g', err);
}

/* @implNote GNU tar's own long-name extension, which predates pax and is what
   nodejs.org's linux tarball still uses: a header of type 'L' or 'K' whose
   PAYLOAD is the name, with the real header following and carrying a truncated
   one. It lands in the same two override slots a pax "path" and "linkpath" use,
   so one mechanism applies a name and the member reader cannot tell which wrote
   it. GNU counts the terminating NUL in the size, and writes more than one when
   it pads, so they are trimmed rather than kept. */
static int read_gnu_long_name(fr_archive *archive, const fr_tar_header *header, fr_error *err) {
    const char *keyword = header->typeflag == 'L' ? "name" : "link name";
    if (header->size > FR_ARCHIVE_MAX_PAX_BYTES) {
        fr_error_set(err, "the archive has a GNU long %s of %llu bytes, and the limit is %d",
                     keyword, header->size, FR_ARCHIVE_MAX_PAX_BYTES);
        return FR_ERR;
    }

    char payload[FR_ARCHIVE_MAX_PAX_BYTES];
    size_t length = (size_t) header->size;
    if (length > 0 && !read_exactly(archive, (unsigned char *) payload, length)) {
        fr_error_set(err, "the archive ends inside a GNU long %s", keyword);
        return FR_ERR;
    }

    size_t padding = (FR_TAR_BLOCK - (length % FR_TAR_BLOCK)) % FR_TAR_BLOCK;
    if (!skip_exactly(archive, padding)) {
        fr_error_set(err, "the archive ends inside a GNU long %s", keyword);
        return FR_ERR;
    }
    archive->offset += length + padding;

    while (length > 0 && payload[length - 1] == '\0') length--;

    char *destination = header->typeflag == 'L' ? archive->pax_name : archive->pax_link_target;
    size_t capacity = header->typeflag == 'L' ? sizeof archive->pax_name
                                              : sizeof archive->pax_link_target;
    if (length == 0 || length >= capacity) {
        fr_error_set(err, "the archive has a GNU long %s of %zu bytes, and the limit is %zu",
                     keyword, length, capacity - 1);
        return FR_ERR;
    }
    memcpy(destination, payload, length);
    destination[length] = '\0';
    return FR_OK;
}

static int is_name_extension(char typeflag) {
    return typeflag == 'x' || typeflag == 'g' || typeflag == 'L' || typeflag == 'K';
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

/* Loops rather than recurses, because a tarball in GNU tar's default posix
   format carries one extended header per member and a recursive reader would
   grow its stack with the archive. D-56. */
static int tar_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    fr_tar_header header;
    for (;;) {
        unsigned char block[FR_TAR_BLOCK];
        if (!read_exactly(archive, block, sizeof block)) {
            fr_error_set(err, "the archive ends before a member header at offset %zu",
                         archive->offset);
            return FR_ERR;
        }

        int end_of_archive = 0;
        if (fr_tar_read_header(block, archive->offset, &header, &end_of_archive, err) != FR_OK) {
            return FR_ERR;
        }
        archive->offset += FR_TAR_BLOCK;
        if (end_of_archive) {
            end_iteration(archive, out_member);
            return FR_OK;
        }
        if (!is_name_extension(header.typeflag)) break;
        if (header.typeflag == 'x' || header.typeflag == 'g') {
            if (read_pax_header(archive, &header, err) != FR_OK) return FR_ERR;
        } else if (read_gnu_long_name(archive, &header, err) != FR_OK) {
            return FR_ERR;
        }
    }

    memcpy(archive->name, header.name, strlen(header.name) + 1);
    memcpy(archive->link_target, header.link_target, sizeof archive->link_target);
    /* Applied AFTER the ustar fields and consumed here, so the next member
       cannot inherit this one's name. */
    if (archive->pax_name[0] != '\0') {
        memcpy(archive->name, archive->pax_name, strlen(archive->pax_name) + 1);
        archive->pax_name[0] = '\0';
    }
    if (archive->pax_link_target[0] != '\0') {
        memcpy(archive->link_target, archive->pax_link_target,
               strlen(archive->pax_link_target) + 1);
        archive->pax_link_target[0] = '\0';
    }

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

    publish(archive, kind, length, (header.mode & 0111u) != 0, (header.mode & 06000u) != 0,
            out_member);
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
    /* The sentinel puts the real size in a zip64 extra field. Reading that
       field is the only way to keep comparing sizes, and a member at or above
       4 GiB is not a toolchain, so the subset is refused instead. */
    if (local_size == ZIP_SIZE_IN_ZIP64_EXTRA) {
        fr_error_set(err, "the archive member \"%s\" is a zip64 entry, which daukle does not read",
                     archive->name);
        return 0;
    }
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

static int zip_setuid(const mz_zip_archive_file_stat *entry) {
    if ((entry->m_version_made_by >> 8) != ZIP_MADE_ON_UNIX) return 0;
    return ((entry->m_external_attr >> 16) & 06000u) != 0;
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
        publish(archive, FR_MEMBER_DIRECTORY, 0, zip_executable(&entry), zip_setuid(&entry),
                out_member);
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

    publish(archive, FR_MEMBER_FILE, length, zip_executable(&entry), zip_setuid(&entry),
            out_member);
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
