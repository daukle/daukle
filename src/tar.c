#include "tar.h"

#include "error.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define NAME_FIELD 0
#define MODE_FIELD 100
#define SIZE_FIELD 124
#define CHECKSUM_FIELD 148
#define TYPEFLAG_FIELD 156
#define LINKNAME_FIELD 157
#define MAGIC_FIELD 257
#define PREFIX_FIELD 345

static int is_zero_block(const unsigned char *block) {
    for (size_t index = 0; index < FR_TAR_BLOCK; index++) {
        if (block[index] != 0) return 0;
    }
    return 1;
}

/* The high-bit test comes first and is the refusal of GNU's base-256 encoding:
   read as octal it would silently produce a different number. */
static int read_octal(const char *field, size_t width, unsigned long long *out) {
    if (((unsigned char) field[0] & 0x80u) != 0) return FR_ERR;

    size_t index = 0;
    while (index < width && field[index] == ' ') index++;

    unsigned long long value = 0;
    int digits = 0;
    while (index < width && field[index] >= '0' && field[index] <= '7') {
        if (value > (ULLONG_MAX - 7) / 8) return FR_ERR;
        value = value * 8 + (unsigned long long) (field[index] - '0');
        index++;
        digits++;
    }
    while (index < width && (field[index] == ' ' || field[index] == '\0')) index++;

    if (digits == 0 || index != width) return FR_ERR;
    *out = value;
    return FR_OK;
}

/* Both sums are accepted because both are produced in the wild: the field is an
   integrity check against a truncated or misaligned read, not a control. */
static int checksum_matches(const unsigned char *block) {
    unsigned long long stored = 0;
    if (read_octal((const char *) block + CHECKSUM_FIELD, 8, &stored) != FR_OK) return 0;

    unsigned long unsigned_sum = 0;
    long signed_sum = 0;
    for (size_t index = 0; index < FR_TAR_BLOCK; index++) {
        unsigned char byte = index >= CHECKSUM_FIELD && index < CHECKSUM_FIELD + 8
                                 ? (unsigned char) ' '
                                 : block[index];
        unsigned_sum += byte;
        signed_sum += (signed char) byte;
    }
    return stored == unsigned_sum || (long long) stored == signed_sum;
}

int fr_tar_looks_like_archive(const char *bytes, size_t length) {
    if (bytes == NULL || length < FR_TAR_BLOCK) return 0;
    const unsigned char *block = (const unsigned char *) bytes;
    if (memcmp(block + MAGIC_FIELD, "ustar", 5) != 0) return 0;
    return checksum_matches(block);
}

/* The trailing "/" + name can reach 155 + 1 + 100 bytes; the +2 over
   FR_TAR_MAX_FULL_NAME leaves room to detect that overflow rather than
   silently truncate it. */
static int assemble_name(const unsigned char *block, fr_tar_header *out, fr_error *err) {
    char raw_name[101];
    memcpy(raw_name, block + NAME_FIELD, 100);
    raw_name[100] = '\0';

    const char *name = raw_name;
    if (strncmp(name, "./", 2) == 0) name += 2;

    char raw_prefix[156];
    memcpy(raw_prefix, block + PREFIX_FIELD, 155);
    raw_prefix[155] = '\0';

    out->used_prefix = raw_prefix[0] != '\0';

    char joined[FR_TAR_MAX_FULL_NAME + 2];
    if (out->used_prefix) {
        snprintf(joined, sizeof joined, "%s/%s", raw_prefix, name);
    } else {
        snprintf(joined, sizeof joined, "%s", name);
    }

    size_t joined_length = strlen(joined);
    if (joined_length == 0) {
        fr_error_set(err, "the archive has a member with no name");
        return FR_ERR;
    }
    if (joined_length > FR_TAR_MAX_FULL_NAME) {
        fr_error_set(err, "the archive member \"%s\" has a name longer than %d bytes", joined,
                     FR_TAR_MAX_FULL_NAME);
        return FR_ERR;
    }
    memcpy(out->name, joined, joined_length + 1);
    return FR_OK;
}

int fr_tar_read_header(const unsigned char *block, fr_tar_header *out, int *out_end_of_archive,
                       fr_error *err) {
    *out_end_of_archive = 0;
    if (is_zero_block(block)) {
        *out_end_of_archive = 1;
        return FR_OK;
    }

    if (!checksum_matches(block)) {
        fr_error_set(err, "the archive has a bad header checksum");
        return FR_ERR;
    }

    if (assemble_name(block, out, err) != FR_OK) return FR_ERR;

    if (read_octal((const char *) block + SIZE_FIELD, 12, &out->size) != FR_OK) {
        fr_error_set(err, "the archive member \"%s\" has a size that is not octal", out->name);
        return FR_ERR;
    }

    unsigned long long mode = 0;
    if (read_octal((const char *) block + MODE_FIELD, 8, &mode) != FR_OK) {
        fr_error_set(err, "the archive member \"%s\" has a mode that is not octal", out->name);
        return FR_ERR;
    }
    out->mode = (unsigned long) mode;

    memcpy(out->link_target, block + LINKNAME_FIELD, 100);
    out->link_target[100] = '\0';

    out->typeflag = (char) block[TYPEFLAG_FIELD];
    return FR_OK;
}

static int already_present(const fr_tar *archive, const char *name) {
    for (size_t index = 0; index < archive->count; index++) {
        if (strcmp(archive->members[index].name, name) == 0) return 1;
    }
    return 0;
}

int fr_tar_read(const char *bytes, size_t length, fr_tar *out, fr_error *err) {
    out->count = 0;

    size_t offset = 0;
    while (offset + FR_TAR_BLOCK <= length) {
        const unsigned char *block = (const unsigned char *) bytes + offset;

        fr_tar_header header;
        int end_of_archive = 0;
        if (fr_tar_read_header(block, &header, &end_of_archive, err) != FR_OK) return FR_ERR;
        if (end_of_archive) return FR_OK;

        /* The plugin reader's own policy, not the parser's limitation: a
           prefixed name would not fit fr_tar_member's storage. */
        if (header.used_prefix) {
            fr_error_set(err, "the archive has a member whose name is too long to store in one"
                              " field");
            return FR_ERR;
        }
        if (strlen(header.name) > FR_TAR_MAX_NAME) {
            fr_error_set(err, "the archive member \"%s\" has a name longer than %d bytes",
                         header.name, FR_TAR_MAX_NAME);
            return FR_ERR;
        }
        /* Bounded before anything else uses it, so every arithmetic below is on
           a number the reader has already agreed to. */
        if (header.size > FR_TAR_MAX_MEMBER_BYTES) {
            fr_error_set(err, "the archive member \"%s\" is larger than %u bytes", header.name,
                         (unsigned) FR_TAR_MAX_MEMBER_BYTES);
            return FR_ERR;
        }

        offset += FR_TAR_BLOCK;

        /* Written as a subtraction because offset + size can wrap, and a wrapped
           comparison is the out-of-bounds read this reader exists not to have. */
        if (header.size > length - offset) {
            fr_error_set(err, "the archive ends inside \"%s\"", header.name);
            return FR_ERR;
        }
        size_t content = offset;
        offset += (size_t) ((header.size + FR_TAR_BLOCK - 1) / FR_TAR_BLOCK) * FR_TAR_BLOCK;

        if (header.typeflag == '5') continue;
        if (header.typeflag != '0' && header.typeflag != '\0') {
            fr_error_set(err, "the archive member \"%s\" is not a regular file (type '%c')",
                         header.name, header.typeflag);
            return FR_ERR;
        }
        if (already_present(out, header.name)) {
            fr_error_set(err, "the archive names \"%s\" twice", header.name);
            return FR_ERR;
        }
        if (out->count == FR_TAR_MAX_MEMBERS) {
            fr_error_set(err, "the archive holds more than %d members", FR_TAR_MAX_MEMBERS);
            return FR_ERR;
        }

        fr_tar_member *member = &out->members[out->count++];
        memcpy(member->name, header.name, strlen(header.name) + 1);
        member->bytes = bytes + content;
        member->length = (size_t) header.size;
    }

    return FR_OK;
}

const fr_tar_member *fr_tar_find(const fr_tar *archive, const char *name) {
    for (size_t index = 0; index < archive->count; index++) {
        if (strcmp(archive->members[index].name, name) == 0) return &archive->members[index];
    }
    return NULL;
}
