#include "archive/archive.h"
#include "archive/archive_internal.h"

#include "util/error.h"

#include "miniz.h"

#include <string.h>

/* A local header and the central directory that disagree are two readers'
   worth of truth about one member, which is the whole parsing differential. */
static int local_header_agrees(fr_archive *archive, const mz_zip_archive_file_stat *entry,
                               fr_error *err) {
    unsigned char header[ZIP_LOCAL_HEADER_SIZE];
    if (!fr_archive_read_at(archive->file, entry->m_local_header_ofs, header, sizeof header)
        || fr_archive_read_little_endian(header, 4) != ZIP_LOCAL_HEADER) {
        fr_error_set(err, "the archive member \"%s\" has no local header", archive->name);
        return 0;
    }

    size_t name_length = (size_t) fr_archive_read_little_endian(header + 26, 2);
    char local_name[FR_ARCHIVE_MAX_NAME + 1];
    if (name_length > FR_ARCHIVE_MAX_NAME
        || !fr_archive_read_at(archive->file, entry->m_local_header_ofs + ZIP_LOCAL_HEADER_SIZE,
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

    unsigned long flags = fr_archive_read_little_endian(header + 6, 2);
    unsigned long local_size = fr_archive_read_little_endian(header + 22, 4);
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

int fr_archive_zip_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err) {
    if (archive->zip_index >= mz_zip_reader_get_num_files(&archive->zip)) {
        fr_archive_end_iteration(archive, out_member);
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
        fr_archive_publish(archive, FR_MEMBER_DIRECTORY, 0, zip_executable(&entry), zip_setuid(&entry),
                           out_member);
        return FR_OK;
    }

    if (fr_archive_refuse_past_cap(archive, entry.m_uncomp_size, err)) return FR_ERR;

    size_t length = (size_t) entry.m_uncomp_size;
    if (!fr_archive_fits_the_member_buffer(archive, length, err)) return FR_ERR;
    if (length > 0
        && !mz_zip_reader_extract_to_mem(&archive->zip, archive->zip_index - 1,
                                         archive->member_bytes, length, 0)) {
        fr_error_set(err, "the archive member \"%s\" cannot be decompressed (%s)", archive->name,
                     mz_zip_get_error_string(mz_zip_get_last_error(&archive->zip)));
        return FR_ERR;
    }
    archive->member_bytes[length] = '\0';

    fr_archive_publish(archive, FR_MEMBER_FILE, length, zip_executable(&entry), zip_setuid(&entry),
                       out_member);
    return FR_OK;
}
