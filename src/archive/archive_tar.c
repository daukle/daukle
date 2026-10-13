#include "archive/archive.h"
#include "archive/archive_internal.h"

#include "archive/tar.h"
#include "util/error.h"

#include "miniz.h"

#include <string.h>

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
    if (length > 0 && !fr_archive_read_exactly(archive, (unsigned char *) payload, length)) {
        fr_error_set(err, "the archive ends inside a pax header");
        return FR_ERR;
    }

    size_t padding = (FR_TAR_BLOCK - (length % FR_TAR_BLOCK)) % FR_TAR_BLOCK;
    if (!fr_archive_skip_exactly(archive, padding)) {
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
    if (length > 0 && !fr_archive_read_exactly(archive, (unsigned char *) payload, length)) {
        fr_error_set(err, "the archive ends inside a GNU long %s", keyword);
        return FR_ERR;
    }

    size_t padding = (FR_TAR_BLOCK - (length % FR_TAR_BLOCK)) % FR_TAR_BLOCK;
    if (!fr_archive_skip_exactly(archive, padding)) {
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
int fr_archive_tar_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    fr_tar_header header;
    for (;;) {
        unsigned char block[FR_TAR_BLOCK];
        if (!fr_archive_read_exactly(archive, block, sizeof block)) {
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
            fr_archive_end_iteration(archive, out_member);
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
    if (fr_archive_refuse_past_cap(archive, header.size, err)) return FR_ERR;

    size_t length = (size_t) header.size;
    if (!fr_archive_fits_the_member_buffer(archive, length, err)) return FR_ERR;
    if (length > 0 && !fr_archive_read_exactly(archive, (unsigned char *) archive->member_bytes, length)) {
        fr_error_set(err, "the archive ends inside \"%s\"", archive->name);
        return FR_ERR;
    }
    archive->member_bytes[length] = '\0';

    size_t padding = (FR_TAR_BLOCK - (length % FR_TAR_BLOCK)) % FR_TAR_BLOCK;
    if (!fr_archive_skip_exactly(archive, padding)) {
        fr_error_set(err, "the archive ends inside \"%s\"", archive->name);
        return FR_ERR;
    }
    archive->offset += length + padding;

    fr_archive_publish(archive, kind, length, (header.mode & 0111u) != 0, (header.mode & 06000u) != 0,
                       out_member);
    return FR_OK;
}
