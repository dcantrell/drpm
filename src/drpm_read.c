/*
    Authors:
        Pavel Tobias <ptobias@redhat.com>
        Matej Chalk <mchalk@redhat.com>

    Copyright (C) 2014 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "drpm.h"
#include "drpm_private.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/md5.h>

#define MAGIC_DRPM 0x6472706D

#define MAGIC_DLT(x) (((x) >> 8) == 0x444C54)
#define MAGIC_DLT3(x) ((x) == 0x444C5433)

static int readdelta_rest(int, struct deltarpm *);
static int readdelta_rpmonly(int, struct deltarpm *);
static int readdelta_standard(int, struct deltarpm *);

/* Reads 32-byte integer in network byte order from file. */
int read_be32(int filedesc, uint32_t *buffer_ret)
{
    unsigned char buffer[4];

    switch (read(filedesc, buffer, 4)) {
    case 4:
        break;
    case -1:
        return DRPM_ERR_IO;
    default:
        return DRPM_ERR_FORMAT;
    }

    *buffer_ret = parse_be32(buffer);

    return DRPM_ERR_OK;
}

/* Reads 64-byte integer in network byte order from file. */
int read_be64(int filedesc, uint64_t *buffer_ret)
{
    unsigned char buffer[8];

    switch (read(filedesc, buffer, 8)) {
    case 8:
        break;
    case -1:
        return DRPM_ERR_IO;
    default:
        return DRPM_ERR_FORMAT;
    }

    *buffer_ret = parse_be64(buffer);

    return DRPM_ERR_OK;
}

/* Reads the rest of the DeltaRPM, i.e. the compressed part
 * that has the same format for standard and rpm-only deltas. */
int readdelta_rest(int filedesc, struct deltarpm *delta)
{
    struct decompstrm *stream;
    uint32_t version;
    uint32_t src_nevr_len;
    uint32_t deltarpm_comp;
    uint32_t offadj_elems_size;
    uint32_t int_copies_size;
    uint32_t ext_copies_size;
    uint32_t ext_data_32;
    uint32_t add_data_len;
    uint32_t int_data_32;
    uint64_t off;
    int error = DRPM_ERR_OK;

    /* initializing decompression and determining compression method */
    if ((error = decompstrm_init(&stream, filedesc, &delta->comp, NULL, NULL, 0)) != DRPM_ERR_OK)
        return error;

    /* reading delta version (1-3) */

    if ((error = decompstrm_read_be32(stream, &version)) != DRPM_ERR_OK)
        goto cleanup;

    if (!MAGIC_DLT(version)) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    delta->version = version % 256 - '0';

    if (delta->version < 3 && delta->type == DRPM_TYPE_RPMONLY) {
        // rpm-only deltas only supported since version 3
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    /* reading source NEVR */

    if ((error = decompstrm_read_be32(stream, &src_nevr_len)) != DRPM_ERR_OK)
        goto cleanup;

    if ((delta->src_nevr = malloc(src_nevr_len + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = decompstrm_read(stream, src_nevr_len, delta->src_nevr)) != DRPM_ERR_OK)
        goto cleanup;

    delta->src_nevr[src_nevr_len] = '\0';

    /* reading DeltaRPM sequence */

    if ((error = decompstrm_read_be32(stream, &delta->sequence_len)) != DRPM_ERR_OK)
        goto cleanup;

    // sequence consists of an MD5 checksum and, for standard deltas,
    // the compressed order in which the files from the RPM header
    // appear in the CPIO archive
    if (delta->sequence_len < MD5_DIGEST_LENGTH ||
        (delta->sequence_len != MD5_DIGEST_LENGTH && delta->type == DRPM_TYPE_RPMONLY)) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((delta->sequence = malloc(delta->sequence_len)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = decompstrm_read(stream, delta->sequence_len, delta->sequence)) != DRPM_ERR_OK)
        goto cleanup;

    /* reading MD5 sum of the target RPM */
    if ((error = decompstrm_read(stream, MD5_DIGEST_LENGTH, delta->tgt_md5)) != DRPM_ERR_OK)
        goto cleanup;

    if (delta->version >= 2) {
        /* reading size of the target RPM and the target compression */
        if ((error = decompstrm_read_be32(stream, &delta->tgt_size)) != DRPM_ERR_OK ||
            (error = decompstrm_read_be32(stream, &deltarpm_comp)) != DRPM_ERR_OK)
            goto cleanup;

        if (!deltarpm_decode_comp(deltarpm_comp, &delta->tgt_comp, &delta->tgt_comp_level)) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }

        /* reading target compression parameters */

        if ((error = decompstrm_read_be32(stream, &delta->tgt_comp_param_len)) != DRPM_ERR_OK)
            goto cleanup;

        if (delta->tgt_comp_param_len > 0) {
            if ((delta->tgt_comp_param = malloc(delta->tgt_comp_param_len)) == NULL) {
                error = DRPM_ERR_MEMORY;
                goto cleanup;
            }

            if ((error = decompstrm_read(stream, delta->tgt_comp_param_len, delta->tgt_comp_param)) != DRPM_ERR_OK)
                goto cleanup;
        }

        if (delta->version == 3) {
            /* reading size of target header included in the diff
             * and the offset adjustment elements for the CPIO archive */
            if ((error = decompstrm_read_be32(stream, &delta->tgt_header_len)) != DRPM_ERR_OK ||
                (error = decompstrm_read_be32(stream, &delta->offadj_elems_count)) != DRPM_ERR_OK)
                goto cleanup;

            if (delta->offadj_elems_count > 0) {
                offadj_elems_size = delta->offadj_elems_count * 2;
                if ((delta->offadj_elems = malloc(offadj_elems_size * 4)) == NULL) {
                    error = DRPM_ERR_MEMORY;
                    goto cleanup;
                }
                for (uint32_t i = 0; i < offadj_elems_size; i += 2)
                    if ((error = decompstrm_read_be32(stream, delta->offadj_elems + i)) != DRPM_ERR_OK)
                        goto cleanup;
                for (uint32_t j = 1; j < offadj_elems_size; j += 2) {
                    if ((error = decompstrm_read_be32(stream, delta->offadj_elems + j)) != DRPM_ERR_OK)
                        goto cleanup;
                    if ((delta->offadj_elems[j] & INT32_MIN) != 0)
                        delta->offadj_elems[j] = TWOS_COMPLEMENT(delta->offadj_elems[j] ^ INT32_MIN);
                }
            }
        }
    }

    if (delta->tgt_header_len == 0 && delta->type == DRPM_TYPE_RPMONLY) {
        // rpm-only deltas include the header in the diff
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    /* reading target lead and signature */

    if ((error = decompstrm_read_be32(stream, &delta->tgt_leadsig_len)) != DRPM_ERR_OK)
        goto cleanup;

    if (delta->tgt_leadsig_len < RPM_LEADSIG_MIN_LEN) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((delta->tgt_leadsig = malloc(delta->tgt_leadsig_len)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = decompstrm_read(stream, delta->tgt_leadsig_len, delta->tgt_leadsig)) != DRPM_ERR_OK)
        goto cleanup;

    /* reading payload format offset and internal and external copies */

    if ((error = decompstrm_read_be32(stream, &delta->payload_fmt_off)) != DRPM_ERR_OK ||
        (error = decompstrm_read_be32(stream, &delta->int_copies_count)) != DRPM_ERR_OK ||
        (error = decompstrm_read_be32(stream, &delta->ext_copies_count)) != DRPM_ERR_OK)
        goto cleanup;

    int_copies_size = delta->int_copies_count * 2;
    ext_copies_size = delta->ext_copies_count * 2;

    if (int_copies_size > 0) {
        if ((delta->int_copies = malloc(int_copies_size * 4)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0; i < int_copies_size; i += 2)
            if ((error = decompstrm_read_be32(stream, delta->int_copies + i)) != DRPM_ERR_OK)
                goto cleanup;
        for (uint32_t j = 1; j < int_copies_size; j += 2)
            if ((error = decompstrm_read_be32(stream, delta->int_copies + j)) != DRPM_ERR_OK)
                goto cleanup;
    }

    if (ext_copies_size > 0) {
        if ((delta->ext_copies = malloc(ext_copies_size * 4)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0; i < ext_copies_size; i += 2) {
            if ((error = decompstrm_read_be32(stream, delta->ext_copies + i)) != DRPM_ERR_OK)
                goto cleanup;
            if ((delta->ext_copies[i] & INT32_MIN) != 0)
                delta->ext_copies[i] = TWOS_COMPLEMENT(delta->ext_copies[i] ^ INT32_MIN);
        }
        for (uint32_t j = 1; j < ext_copies_size; j += 2)
            if ((error = decompstrm_read_be32(stream, delta->ext_copies + j)) != DRPM_ERR_OK)
                goto cleanup;
    }

    /* reading length of external data */
    if (delta->version == 3) {
        if ((error = decompstrm_read_be64(stream, &delta->ext_data_len)) != DRPM_ERR_OK)
            goto cleanup;
    } else {
        if ((error = decompstrm_read_be32(stream, &ext_data_32)) != DRPM_ERR_OK)
            goto cleanup;
        delta->ext_data_len = ext_data_32;
    }

    /* reading add data */

    if ((error = decompstrm_read_be32(stream, &add_data_len)) != DRPM_ERR_OK)
        goto cleanup;

    if (add_data_len > 0) {
        if (delta->type == DRPM_TYPE_RPMONLY) {
            // should be empty for rpm-only deltas, as they include this earlier
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
        if ((delta->add_data = malloc(add_data_len)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        if ((error = decompstrm_read(stream, add_data_len, delta->add_data)) != DRPM_ERR_OK)
            goto cleanup;
        delta->add_data_len = add_data_len;
    }

    /* reading internal data */

    if (delta->version == 3) {
        if ((error = decompstrm_read_be64(stream, &delta->int_data_len)) != DRPM_ERR_OK)
            goto cleanup;
    } else {
        if ((error = decompstrm_read_be32(stream, &int_data_32)) != DRPM_ERR_OK)
            goto cleanup;
        delta->int_data_len = int_data_32;
    }

    if (delta->int_data_len > SIZE_MAX) {
        error = DRPM_ERR_OVERFLOW;
        goto cleanup;
    }

    if (delta->int_data_len > 0) {
        if ((delta->int_data.bytes = malloc(delta->int_data_len)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        if ((error = decompstrm_read(stream, delta->int_data_len, delta->int_data.bytes)) != DRPM_ERR_OK)
            goto cleanup;
    }

    delta->int_data_as_ptrs = false;

    /* checking internal copies against internal data length */
    off = 0;
    for (uint32_t i = 1; i < int_copies_size; i += 2) {
        off += delta->int_copies[i];
        if (off > delta->int_data_len) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
    }

    /* checking external copies against external data length */
    off = 0;
    for (uint32_t i = 0; i < ext_copies_size; i += 2) {
        off += (int32_t)delta->ext_copies[i];
        if (off > delta->ext_data_len) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
        off += delta->ext_copies[i + 1];
        if (off == 0 || off > delta->ext_data_len) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
    }

cleanup:
    decompstrm_destroy(&stream);

    return error;
}

/* Reads part of DeltaRPM specific to rpm-only deltas. */
int readdelta_rpmonly(int filedesc, struct deltarpm *delta)
{
    uint32_t version;
    uint32_t tgt_nevr_len;
    ssize_t bytes_read;
    int error;

    if ((error = read_be32(filedesc, &version)) != DRPM_ERR_OK)
        return error;

    if (!MAGIC_DLT3(version))
        return DRPM_ERR_FORMAT;

    if ((error = read_be32(filedesc, &tgt_nevr_len)) != DRPM_ERR_OK)
        return error;

    /* reading target NEVR */

    if ((delta->head.tgt_nevr = malloc(tgt_nevr_len + 1)) == NULL)
        return DRPM_ERR_MEMORY;

    if ((bytes_read = read(filedesc, delta->head.tgt_nevr, tgt_nevr_len)) < 0)
        return DRPM_ERR_IO;

    if ((uint32_t)bytes_read != tgt_nevr_len)
        return DRPM_ERR_FORMAT;

    delta->head.tgt_nevr[tgt_nevr_len] = '\0';

    /* reading add data */

    if ((error = read_be32(filedesc, &delta->add_data_len)) != DRPM_ERR_OK)
        return error;

    if ((delta->add_data = malloc(delta->add_data_len)) == NULL)
        return DRPM_ERR_MEMORY;

    if ((bytes_read = read(filedesc, delta->add_data, delta->add_data_len)) < 0)
        return DRPM_ERR_IO;

    if ((uint32_t)bytes_read != delta->add_data_len)
        return DRPM_ERR_FORMAT;

    return DRPM_ERR_OK;
}

/* Reads part of DeltaRPM specific to standard deltas. */
int readdelta_standard(int filedesc, struct deltarpm *delta)
{
    struct rpm *rpmst;
    int error;

    /* reading RPM lead, signature and header */
    if ((error = rpm_read(&rpmst, delta->filename, RPM_ARCHIVE_DONT_READ, NULL, NULL, NULL)) != DRPM_ERR_OK)
        return error;

    /* reading target compression from header (used for older delta versions) */
    if ((error = rpm_get_comp(rpmst, &delta->tgt_comp)) != DRPM_ERR_OK)
        return error;

    if (lseek(filedesc, rpm_size_full(rpmst), SEEK_SET) == (off_t)-1)
        return DRPM_ERR_IO;

    delta->head.tgt_rpm = rpmst;

    return DRPM_ERR_OK;
}

/* Reads DeltaRPM from file. */
int read_deltarpm(struct deltarpm *delta, const char *filename)
{
    int filedesc;
    uint32_t magic;
    int error = DRPM_ERR_OK;

    if (filename == NULL || delta == NULL)
        return DRPM_ERR_PROG;

    if ((filedesc = open(filename, O_RDONLY)) == -1)
        return DRPM_ERR_IO;

    delta->filename = filename;

    /* determining type of delta by magic bytes and calling relevant subroutine */

    if ((error = read_be32(filedesc, &magic)) != DRPM_ERR_OK)
        goto cleanup_fail;

    switch (magic) {
    case MAGIC_DRPM:
        delta->type = DRPM_TYPE_RPMONLY;
        if ((error = readdelta_rpmonly(filedesc, delta)) != DRPM_ERR_OK)
            goto cleanup_fail;
        break;
    case MAGIC_RPM:
        delta->type = DRPM_TYPE_STANDARD;
        if ((error = readdelta_standard(filedesc, delta)) != DRPM_ERR_OK)
            goto cleanup_fail;
        break;
    default:
        error = DRPM_ERR_FORMAT;
        goto cleanup_fail;
    }

    /* the rest of the delta is the same for both types */
    if ((error = readdelta_rest(filedesc, delta)) != DRPM_ERR_OK)
        goto cleanup_fail;

    goto cleanup;

cleanup_fail:
    free_deltarpm(delta);

cleanup:
    close(filedesc);

    return error;
}

/* Converts DeltaRPM data to more readable format. */
int deltarpm_to_drpm(const struct deltarpm *src, struct drpm *dst)
{
    const struct drpm init = {0};
    int error;

    if (src == NULL || dst == NULL)
        return DRPM_ERR_PROG;

    *dst = init;

    dst->version = src->version;
    dst->type = src->type;
    dst->comp = src->comp;
    dst->tgt_size = src->tgt_size;
    dst->tgt_comp = src->tgt_comp;
    dst->tgt_header_len = src->tgt_header_len;
    dst->payload_fmt_off = src->payload_fmt_off;
    dst->ext_data_len = src->ext_data_len;
    dst->int_data_len = src->int_data_len;

    dst->offadj_elems_size = src->offadj_elems_count * 2;
    dst->int_copies_size = src->int_copies_count * 2;
    dst->ext_copies_size = src->ext_copies_count * 2;

    if ((dst->filename = malloc(strlen(src->filename) + 1)) == NULL ||
        (dst->sequence = malloc(src->sequence_len * 2 + 1)) == NULL ||
        (dst->src_nevr = malloc(strlen(src->src_nevr) + 1)) == NULL ||
        (src->tgt_comp_param_len > 0 &&
         (dst->tgt_comp_param = malloc(src->tgt_comp_param_len * 2 + 1)) == NULL) ||
        (dst->tgt_leadsig = malloc(src->tgt_leadsig_len * 2 + 1)) == NULL ||
        (dst->offadj_elems_size > 0 &&
         (dst->offadj_elems = malloc(dst->offadj_elems_size * 4)) == NULL) ||
        (dst->int_copies_size > 0 &&
         (dst->int_copies = malloc(dst->int_copies_size * 4)) == NULL) ||
        (dst->ext_copies_size > 0 &&
         (dst->ext_copies = malloc(dst->ext_copies_size * 4)) == NULL)) {
        error = DRPM_ERR_MEMORY;
        goto cleanup_fail;
    }

    strcpy(dst->filename, src->filename);
    strcpy(dst->src_nevr, src->src_nevr);

    dump_hex(dst->sequence, src->sequence, src->sequence_len);
    dump_hex(dst->tgt_md5, src->tgt_md5, MD5_DIGEST_LENGTH);
    dump_hex(dst->tgt_leadsig, src->tgt_leadsig, src->tgt_leadsig_len);
    if (src->tgt_comp_param_len > 0)
        dump_hex(dst->tgt_comp_param, src->tgt_comp_param, src->tgt_comp_param_len);

    if (dst->offadj_elems_size > 0)
        memcpy(dst->offadj_elems, src->offadj_elems, dst->offadj_elems_size * 4);
    if (dst->int_copies_size > 0)
        memcpy(dst->int_copies, src->int_copies, dst->int_copies_size * 4);
    if (dst->ext_copies_size > 0)
        memcpy(dst->ext_copies, src->ext_copies, dst->ext_copies_size * 4);

    if (src->type == DRPM_TYPE_STANDARD) {
        if ((error = rpm_get_nevr(src->head.tgt_rpm, &dst->tgt_nevr)) != DRPM_ERR_OK)
            goto cleanup_fail;
    } else {
        if ((dst->tgt_nevr = malloc(strlen(src->head.tgt_nevr) + 1)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup_fail;
        }
        strcpy(dst->tgt_nevr, src->head.tgt_nevr);
    }

    return DRPM_ERR_OK;

cleanup_fail:
    drpm_free(dst);

    return error;
}

void drpm_free(struct drpm *delta)
{
    const struct drpm delta_init = {0};

    free(delta->filename);
    free(delta->src_nevr);
    free(delta->tgt_nevr);
    free(delta->sequence);
    free(delta->tgt_comp_param);
    free(delta->tgt_leadsig);
    free(delta->offadj_elems);
    free(delta->int_copies);
    free(delta->ext_copies);

    *delta = delta_init;
}
