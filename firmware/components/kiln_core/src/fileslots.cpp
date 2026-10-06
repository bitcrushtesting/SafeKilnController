/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Named blobs over a raw flash partition.  The layout, and why the commit
 * header is written second, are in fileslots.h.
 */

#include <string.h>

#include "kiln_core/fileslots.h"
#include "kiln_core/logrec.h"   /* kiln_crc16_update */

/* 'K','F','S','1' read back little endian. */
constexpr uint32_t FS_MAGIC = 0x3153464Bu;

constexpr uint32_t OFF_MAGIC = 0u;
constexpr uint32_t OFF_SEQ   = 4u;
constexpr uint32_t OFF_LEN   = 8u;
constexpr uint32_t OFF_CRC   = 10u;
constexpr uint32_t OFF_NAME  = 12u;
#define OFF_PAYLOAD KILN_FILESLOT_HDR

constexpr uint32_t SEQ_ERASED = 0xFFFFFFFFu;

/* --- little endian accessors -------------------------------------------- */

namespace {

uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8u));
}

void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
    p[2] = (uint8_t)(v >> 16u);
    p[3] = (uint8_t)(v >> 24u);
}

void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
}

/* --- geometry ------------------------------------------------------------ */

uint32_t copy_off(const kiln_fileslots_t *fs, uint16_t region, uint8_t copy)
{
    return ((uint32_t)region * fs->region_bytes) + ((uint32_t)copy * fs->sector_bytes);
}

/* --- one copy ------------------------------------------------------------ */

/* True if the copy at (region, copy) is a complete, committed file.  The CRC is
 * streamed so that validating a mount costs a 64 byte buffer and not a sector. */
bool read_copy(const kiln_fileslots_t *fs, uint16_t region, uint8_t copy,
                      kiln_fileslot_entry_t *out)
{
    uint8_t head[KILN_FILESLOT_HDR];
    const uint32_t base = copy_off(fs, region, copy);

    if (fs->flash->read(fs->flash->ctx, base, head, sizeof(head)) != KILN_OK) {
        return false;
    }
    if (get_u32(&head[OFF_MAGIC]) != FS_MAGIC) {
        return false;   /* erased, or the commit never happened */
    }

    const uint32_t seq = get_u32(&head[OFF_SEQ]);
    const uint16_t len = get_u16(&head[OFF_LEN]);
    if ((seq == SEQ_ERASED) || (seq == 0u) || (len > fs->payload_max)) {
        return false;
    }
    /* The name must terminate inside its field, or it is not a name. */
    if (memchr(&head[OFF_NAME], 0, KILN_PATH_MAX) == nullptr) {
        return false;
    }

    uint16_t crc = kiln_crc16_update(KILN_CRC16_INIT, &head[OFF_NAME], KILN_PATH_MAX);
    uint32_t off = base + OFF_PAYLOAD;
    uint16_t left = len;
    while (left > 0u) {
        uint8_t buf[64];
        const uint16_t n = (left > sizeof(buf)) ? (uint16_t)sizeof(buf) : left;
        if (fs->flash->read(fs->flash->ctx, off, buf, n) != KILN_OK) {
            return false;
        }
        crc = kiln_crc16_update(crc, buf, n);
        off  += n;
        left = (uint16_t)(left - n);
    }
    if (crc != get_u16(&head[OFF_CRC])) {
        return false;   /* cut during the commit header, or bit rot */
    }

    memcpy(out->name, &head[OFF_NAME], KILN_PATH_MAX);
    out->seq  = seq;
    out->len  = len;
    out->crc  = crc;
    out->copy = copy;
    out->used = true;
    return true;
}

/* --- index --------------------------------------------------------------- */

uint16_t find_region(const kiln_fileslots_t *fs, const char *name)
{
    for (uint16_t r = 0; r < fs->region_count; r++) {
        if (fs->entry[r].used && (strcmp(fs->entry[r].name, name) == 0)) {
            return r;
        }
    }
    return fs->region_count;
}

uint16_t free_region(const kiln_fileslots_t *fs)
{
    for (uint16_t r = 0; r < fs->region_count; r++) {
        if (!fs->entry[r].used) {
            return r;
        }
    }
    return fs->region_count;
}

} // namespace

kiln_err_t kiln_fileslots_mount(kiln_fileslots_t *fs, const kiln_port_flash_t *flash)
{
    if ((fs == nullptr) || (flash == nullptr) || (flash->read == nullptr) ||
        (flash->write == nullptr) || (flash->erase == nullptr) ||
        (flash->info == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    memset(fs, 0, sizeof(*fs));

    kiln_flash_info_t info = {};
    const kiln_err_t e = flash->info(flash->ctx, &info);
    if (e != KILN_OK) {
        return e;
    }
    if ((info.sector_bytes <= KILN_FILESLOT_HDR) || (info.size_bytes == 0u)) {
        return KILN_ERR_UNSUPPORTED;
    }

    fs->flash        = flash;
    fs->sector_bytes = info.sector_bytes;
    fs->region_bytes = info.sector_bytes * 2u;
    fs->payload_max  = (uint16_t)((info.sector_bytes - KILN_FILESLOT_HDR) & ~3u);

    uint32_t regions = info.size_bytes / fs->region_bytes;
    if (regions > KILN_FILESLOT_REGIONS_MAX) {
        regions = KILN_FILESLOT_REGIONS_MAX;
    }
    if (regions == 0u) {
        return KILN_ERR_UNSUPPORTED;   /* smaller than one file */
    }
    fs->region_count = (uint16_t)regions;

    for (uint16_t r = 0; r < fs->region_count; r++) {
        for (uint8_t c = 0; c < 2u; c++) {
            kiln_fileslot_entry_t cand = {};
            if (!read_copy(fs, r, c, &cand)) {
                continue;
            }
            /* Both copies valid happens when a write committed and the previous
             * copy was left to be erased by the next write.  Higher seq wins. */
            if (!fs->entry[r].used || (cand.seq > fs->entry[r].seq)) {
                fs->entry[r] = cand;
            }
        }
    }

    fs->mounted = true;
    return KILN_OK;
}

kiln_err_t kiln_fileslots_format(kiln_fileslots_t *fs)
{
    if ((fs == nullptr) || !fs->mounted) {
        return KILN_ERR_STATE;
    }
    for (uint16_t r = 0; r < fs->region_count; r++) {
        const kiln_err_t e = fs->flash->erase(fs->flash->ctx,
                                              copy_off(fs, r, 0), fs->region_bytes);
        if (e != KILN_OK) {
            return e;
        }
        fs->entry[r] = kiln_fileslot_entry_t{};
    }
    return KILN_OK;
}

uint16_t kiln_fileslots_used_regions(const kiln_fileslots_t *fs)
{
    if ((fs == nullptr) || !fs->mounted) {
        return 0u;
    }
    uint16_t n = 0;
    for (uint16_t r = 0; r < fs->region_count; r++) {
        if (fs->entry[r].used) {
            n++;
        }
    }
    return n;
}

/* --- the port ------------------------------------------------------------ */

namespace {

bool name_ok(const char *path)
{
    return (path != nullptr) && (path[0] != '\0') &&
           (strnlen(path, KILN_PATH_MAX) < KILN_PATH_MAX);
}

kiln_err_t fs_read(void *ctx, const char *path, void *out, size_t cap, size_t *len)
{
    const kiln_fileslots_t *fs = static_cast<const kiln_fileslots_t *>(ctx);
    if ((fs == nullptr) || !fs->mounted || !name_ok(path) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    const uint16_t r = find_region(fs, path);
    if (r == fs->region_count) {
        return KILN_ERR_NOT_FOUND;
    }
    const kiln_fileslot_entry_t *en = &fs->entry[r];
    if (en->len > cap) {
        return KILN_ERR_RANGE;
    }

    const uint32_t off = copy_off(fs, r, en->copy) + OFF_PAYLOAD;
    const kiln_err_t e = fs->flash->read(fs->flash->ctx, off, out, en->len);
    if (e != KILN_OK) {
        return e;
    }
    /* Re-check against the committed CRC: mount proved it once, and a read is
     * the last chance to notice that the medium has changed since. */
    /* The name is a char array and the CRC reads bytes; nothing here depends on
     * char's signedness, because the same bytes went in on write, which is what
     * makes the comparison below valid. */
    uint16_t crc = kiln_crc16_update(KILN_CRC16_INIT, kiln_bytes_of(en->name),
                                     KILN_PATH_MAX);
    crc = kiln_crc16_update(crc, static_cast<const uint8_t *>(out), en->len);
    if (crc != en->crc) {
        return KILN_ERR_CORRUPT;
    }
    if (len != nullptr) {
        *len = en->len;
    }
    return KILN_OK;
}

kiln_err_t fs_write_atomic(void *ctx, const char *path, const void *data, size_t len)
{
    kiln_fileslots_t *fs = static_cast<kiln_fileslots_t *>(ctx);
    if ((fs == nullptr) || !fs->mounted || !name_ok(path) ||
        ((data == nullptr) && (len > 0u))) {
        return KILN_ERR_INVALID_ARG;
    }
    if (len > fs->payload_max) {
        return KILN_ERR_RANGE;
    }

    uint16_t r      = find_region(fs, path);
    const bool fresh = (r == fs->region_count);
    if (fresh) {
        r = free_region(fs);
        if (r == fs->region_count) {
            return KILN_ERR_NO_SPACE;
        }
    }

    const uint8_t  target = fresh ? 0u : (uint8_t)(1u - fs->entry[r].copy);
    const uint32_t seq    = fresh ? 1u : (fs->entry[r].seq + 1u);
    const uint32_t base   = copy_off(fs, r, target);

    kiln_err_t e = fs->flash->erase(fs->flash->ctx, base, fs->sector_bytes);
    if (e != KILN_OK) {
        return e;
    }

    /* The name field is written in full so that its 64 bytes are what the CRC
     * covers on both sides, whatever the caller's string length. */
    uint8_t namebuf[KILN_PATH_MAX];
    memset(namebuf, 0, sizeof(namebuf));
    memcpy(namebuf, path, strnlen(path, KILN_PATH_MAX - 1u));

    e = fs->flash->write(fs->flash->ctx, base + OFF_NAME, namebuf, sizeof(namebuf));
    if (e != KILN_OK) {
        return e;
    }

    /* Flash writes are 4 byte aligned; the odd tail goes down in its own word
     * padded with the erased value, which the CRC does not cover. */
    const size_t whole = len & ~(size_t)3u;
    if (whole > 0u) {
        e = fs->flash->write(fs->flash->ctx, base + OFF_PAYLOAD, data, whole);
        if (e != KILN_OK) {
            return e;
        }
    }
    if (len > whole) {
        uint8_t tail[4];
        memset(tail, 0xFF, sizeof(tail));
        memcpy(tail, static_cast<const uint8_t *>(data) + whole, len - whole);
        e = fs->flash->write(fs->flash->ctx, base + OFF_PAYLOAD + whole, tail, sizeof(tail));
        if (e != KILN_OK) {
            return e;
        }
    }

    uint16_t crc = kiln_crc16_update(KILN_CRC16_INIT, namebuf, sizeof(namebuf));
    crc = kiln_crc16_update(crc, static_cast<const uint8_t *>(data), len);

    /* The commit.  Until these twelve bytes land the copy above does not exist,
     * and the previous copy is still the one with the highest seq. */
    uint8_t head[OFF_NAME];
    put_u32(&head[OFF_MAGIC], FS_MAGIC);
    put_u32(&head[OFF_SEQ], seq);
    put_u16(&head[OFF_LEN], (uint16_t)len);
    put_u16(&head[OFF_CRC], crc);
    e = fs->flash->write(fs->flash->ctx, base + OFF_MAGIC, head, sizeof(head));
    if (e != KILN_OK) {
        return e;
    }

    kiln_fileslot_entry_t *en = &fs->entry[r];
    memcpy(en->name, namebuf, sizeof(namebuf));
    en->seq  = seq;
    en->len  = (uint16_t)len;
    en->crc  = crc;
    en->copy = target;
    en->used = true;
    return KILN_OK;
}

kiln_err_t fs_remove(void *ctx, const char *path)
{
    kiln_fileslots_t *fs = static_cast<kiln_fileslots_t *>(ctx);
    if ((fs == nullptr) || !fs->mounted || !name_ok(path)) {
        return KILN_ERR_INVALID_ARG;
    }
    const uint16_t r = find_region(fs, path);
    if (r == fs->region_count) {
        return KILN_ERR_NOT_FOUND;
    }
    const kiln_err_t e = fs->flash->erase(fs->flash->ctx,
                                          copy_off(fs, r, 0), fs->region_bytes);
    if (e != KILN_OK) {
        return e;
    }
    fs->entry[r] = kiln_fileslot_entry_t{};
    return KILN_OK;
}

kiln_err_t fs_exists(void *ctx, const char *path)
{
    const kiln_fileslots_t *fs = static_cast<const kiln_fileslots_t *>(ctx);
    if ((fs == nullptr) || !fs->mounted || !name_ok(path)) {
        return KILN_ERR_INVALID_ARG;
    }
    return (find_region(fs, path) == fs->region_count) ? KILN_ERR_NOT_FOUND : KILN_OK;
}

kiln_err_t fs_list(void *ctx, const char *dir,
                          bool (*fn)(void *user, const char *name, size_t size),
                          void *user)
{
    const kiln_fileslots_t *fs = static_cast<const kiln_fileslots_t *>(ctx);
    if ((fs == nullptr) || !fs->mounted || (fn == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    const size_t pre = (dir != nullptr) ? strnlen(dir, KILN_PATH_MAX) : 0u;
    for (uint16_t r = 0; r < fs->region_count; r++) {
        const kiln_fileslot_entry_t *en = &fs->entry[r];
        if (!en->used) {
            continue;
        }
        if ((pre > 0u) && (strncmp(en->name, dir, pre) != 0)) {
            continue;
        }
        if (!fn(user, en->name, en->len)) {
            break;
        }
    }
    return KILN_OK;
}

kiln_err_t fs_usage(void *ctx, size_t *total, size_t *used)
{
    const kiln_fileslots_t *fs = static_cast<const kiln_fileslots_t *>(ctx);
    if ((fs == nullptr) || !fs->mounted) {
        return KILN_ERR_STATE;
    }
    if (total != nullptr) {
        *total = (size_t)fs->region_count * fs->payload_max;
    }
    if (used != nullptr) {
        size_t n = 0;
        for (uint16_t r = 0; r < fs->region_count; r++) {
            if (fs->entry[r].used) {
                n += fs->entry[r].len;
            }
        }
        *used = n;
    }
    return KILN_OK;
}

} // namespace

void kiln_fileslots_bind(kiln_fileslots_t *fs, kiln_port_filestore_t *out)
{
    if ((fs == nullptr) || (out == nullptr)) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->ctx          = fs;
    out->read         = fs_read;
    out->write_atomic = fs_write_atomic;
    out->remove       = fs_remove;
    out->exists       = fs_exists;
    out->list         = fs_list;
    out->usage        = fs_usage;
}
