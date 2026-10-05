/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>
#include "kiln_app/program_store.h"
#include "kiln_core/logrec.h"      /* kiln_crc16 */
#include "kiln_core/profile.h"

#define PROG_MAGIC       0x47525021U   /* "!PRG" */
#define PROG_BLOB_BYTES  (8u + sizeof(kiln_program_t) + 2u)

static void slot_path(uint8_t slot, char out[KILN_PATH_MAX])
{
    snprintf(out, KILN_PATH_MAX, "/p/%02u", (unsigned)slot);
}

static kiln_err_t encode(const kiln_program_t *p, uint8_t *out, size_t cap, size_t *len)
{
    if (cap < PROG_BLOB_BYTES) {
        return KILN_ERR_NO_SPACE;
    }

    memset(out, 0, PROG_BLOB_BYTES);
    out[0] = (uint8_t)PROG_MAGIC;
    out[1] = (uint8_t)(PROG_MAGIC >> 8);
    out[2] = (uint8_t)(PROG_MAGIC >> 16);
    out[3] = (uint8_t)(PROG_MAGIC >> 24);
    out[4] = (uint8_t)KILN_PROGRAM_SCHEMA_VERSION;
    out[5] = (uint8_t)(sizeof(kiln_program_t));
    out[6] = (uint8_t)(sizeof(kiln_program_t) >> 8);
    out[7] = 0;
    memcpy(&out[8], p, sizeof(*p));

    const uint16_t crc = kiln_crc16(out, PROG_BLOB_BYTES - 2);
    out[PROG_BLOB_BYTES - 2] = (uint8_t)crc;
    out[PROG_BLOB_BYTES - 1] = (uint8_t)(crc >> 8);

    *len = PROG_BLOB_BYTES;
    return KILN_OK;
}

static kiln_err_t decode(const uint8_t *in, size_t len, kiln_program_t *out)
{
    if (len != PROG_BLOB_BYTES) {
        return KILN_ERR_CORRUPT;
    }

    const uint32_t magic = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (magic != PROG_MAGIC) {
        return KILN_ERR_CORRUPT;
    }

    const uint16_t crc = (uint16_t)(in[PROG_BLOB_BYTES - 2] |
                                    ((uint16_t)in[PROG_BLOB_BYTES - 1] << 8));
    if (kiln_crc16(in, PROG_BLOB_BYTES - 2) != crc) {
        return KILN_ERR_CORRUPT;
    }

    memcpy(out, &in[8], sizeof(*out));

    /* NFR-19: this came off storage, so it is untrusted in exactly the way a
     * network body is.  Force termination before anything reads the strings. */
    kiln_profile_terminate_strings(out);
    return KILN_OK;
}

static kiln_err_t read_slot(const kiln_port_filestore_t *fs, uint8_t slot,
                            kiln_program_t *out)
{
    char path[KILN_PATH_MAX];
    slot_path(slot, path);

    uint8_t blob[PROG_BLOB_BYTES];
    size_t  len = 0;
    const kiln_err_t e = fs->read(fs->ctx, path, blob, sizeof(blob), &len);
    if (e != KILN_OK) {
        return e;
    }
    return decode(blob, len, out);
}

static kiln_err_t write_slot(const kiln_port_filestore_t *fs, uint8_t slot,
                             const kiln_program_t *p)
{
    uint8_t blob[PROG_BLOB_BYTES];
    size_t  len = 0;
    const kiln_err_t e = encode(p, blob, sizeof(blob), &len);
    if (e != KILN_OK) {
        return e;
    }

    char path[KILN_PATH_MAX];
    slot_path(slot, path);
    /* Atomic by rename, so a power cut leaves the previous program rather than
     * half of the new one. */
    return fs->write_atomic(fs->ctx, path, blob, len);
}

/* Slot holding `name`, or KILN_PROGRAM_SLOTS if there is none. */
static uint8_t find_by_name(const kiln_port_filestore_t *fs, const char *name,
                            bool *readonly_out)
{
    for (uint8_t slot = 0; slot < KILN_PROGRAM_SLOTS; slot++) {
        kiln_program_t p;
        if (read_slot(fs, slot, &p) != KILN_OK) {
            continue;
        }
        if (strncmp(p.name, name, KILN_PROGRAM_NAME_LEN) == 0) {
            if (readonly_out != nullptr) {
                *readonly_out = (p.flags & KILN_PROG_FLAG_READONLY) != 0;
            }
            return slot;
        }
    }
    return KILN_PROGRAM_SLOTS;
}

static uint8_t first_free_slot(const kiln_port_filestore_t *fs)
{
    for (uint8_t slot = 0; slot < KILN_PROGRAM_SLOTS; slot++) {
        kiln_program_t p;
        if (read_slot(fs, slot, &p) != KILN_OK) {
            return slot;
        }
    }
    return KILN_PROGRAM_SLOTS;
}

kiln_err_t kiln_program_store_seed(const kiln_port_filestore_t *fs)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (fs->write_atomic == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const uint8_t n = kiln_profile_example_count();
    for (uint8_t i = 0; i < n; i++) {
        kiln_program_t p;
        kiln_err_t e = kiln_profile_example(i, &p);
        if (e != KILN_OK) {
            return e;
        }

        /* Idempotent: an example the operator has not deleted is left exactly
         * as it is, so seeding on every boot costs one read per example. */
        bool ro = false;
        if (find_by_name(fs, p.name, &ro) != KILN_PROGRAM_SLOTS) {
            continue;
        }

        const uint8_t slot = first_free_slot(fs);
        if (slot >= KILN_PROGRAM_SLOTS) {
            return KILN_ERR_NO_SPACE;
        }

        e = write_slot(fs, slot, &p);
        if (e != KILN_OK) {
            return e;
        }
    }
    return KILN_OK;
}

kiln_err_t kiln_program_store_save(const kiln_port_filestore_t *fs,
                                   const kiln_program_t *p, float max_temp_c)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (fs->write_atomic == nullptr) ||
        (p == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* FR-PRG-05 before anything reaches storage. */
    const kiln_prog_validation_t v = kiln_profile_validate(p, max_temp_c);
    if (v.code != KILN_PROG_OK) {
        return KILN_ERR_RANGE;
    }

    bool ro = false;
    uint8_t slot = find_by_name(fs, p->name, &ro);
    if (slot < KILN_PROGRAM_SLOTS) {
        /* FR-PRG-09: the built-in examples are read-only.  Saving over one would
         * leave the operator with no way back to a known-good program. */
        if (ro) {
            return KILN_ERR_STATE;
        }
    } else {
        slot = first_free_slot(fs);
        if (slot >= KILN_PROGRAM_SLOTS) {
            return KILN_ERR_NO_SPACE;
        }
    }
    return write_slot(fs, slot, p);
}

kiln_err_t kiln_program_store_load(const kiln_port_filestore_t *fs,
                                   const char *name, kiln_program_t *out)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (name == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    for (uint8_t slot = 0; slot < KILN_PROGRAM_SLOTS; slot++) {
        kiln_program_t p;
        if (read_slot(fs, slot, &p) != KILN_OK) {
            continue;
        }
        if (strncmp(p.name, name, KILN_PROGRAM_NAME_LEN) == 0) {
            *out = p;
            return KILN_OK;
        }
    }
    return KILN_ERR_NOT_FOUND;
}

kiln_err_t kiln_program_store_delete(const kiln_port_filestore_t *fs, const char *name)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (fs->remove == nullptr) || (name == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    bool ro = false;
    const uint8_t slot = find_by_name(fs, name, &ro);
    if (slot >= KILN_PROGRAM_SLOTS) {
        return KILN_ERR_NOT_FOUND;
    }
    if (ro) {
        return KILN_ERR_STATE; /* FR-PRG-09 */
    }

    char path[KILN_PATH_MAX];
    slot_path(slot, path);
    return fs->remove(fs->ctx, path);
}

kiln_err_t kiln_program_store_get_slot(const kiln_port_filestore_t *fs,
                                       uint8_t slot, kiln_program_t *out)
{
    if ((fs == nullptr) || (fs->read == nullptr) || (out == nullptr) ||
        slot >= KILN_PROGRAM_SLOTS) {
        return KILN_ERR_INVALID_ARG;
    }
    return read_slot(fs, slot, out);
}

uint8_t kiln_program_store_count(const kiln_port_filestore_t *fs)
{
    if ((fs == nullptr) || (fs->read == nullptr)) {
        return 0;
    }

    uint8_t n = 0;
    for (uint8_t slot = 0; slot < KILN_PROGRAM_SLOTS; slot++) {
        kiln_program_t p;
        if (read_slot(fs, slot, &p) == KILN_OK) {
            n++;
        }
    }
    return n;
}
