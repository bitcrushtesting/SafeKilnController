/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>
#include "kiln_hal_host/hal_host.h"

/* --- flash ------------------------------------------------------------- */

void kiln_host_flash_init(kiln_host_flash_t *f, uint8_t *storage,
                          uint32_t size_bytes, uint32_t sector_bytes)
{
    if ((f == nullptr) || (storage == nullptr)) {
        return;
    }

    memset(f, 0, sizeof(*f));
    f->data         = storage;
    f->size_bytes   = size_bytes;
    f->sector_bytes = sector_bytes;
    f->powered      = true;
    /* Not zero: zero is the first sector's offset, and a fault injector that is
     * armed by default would be found the hard way. */
    f->fail_erase_off = KILN_HOST_NO_OFFSET;
    memset(storage, 0xFF, size_bytes);
}

void kiln_host_flash_power_on(kiln_host_flash_t *f)
{
    if (f != nullptr) {
        f->powered = true;
    }
}

namespace {

bool in_range(const kiln_host_flash_t *f, uint32_t off, size_t len)
{
    return (uint64_t)off + len <= (uint64_t)f->size_bytes;
}

kiln_err_t hf_info(void *ctx, kiln_flash_info_t *out)
{
    const kiln_host_flash_t *f = static_cast<const kiln_host_flash_t *>(ctx);
    if ((f == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    out->size_bytes   = f->size_bytes;
    out->sector_bytes = f->sector_bytes;
    return KILN_OK;
}

kiln_err_t hf_read(void *ctx, uint32_t off, void *out, size_t len)
{
    kiln_host_flash_t *f = static_cast<kiln_host_flash_t *>(ctx);
    if ((f == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!f->powered) {
        return KILN_ERR_IO;
    }
    if (!in_range(f, off, len)) {
        return KILN_ERR_INVALID_ARG;
    }

    memcpy(out, &f->data[off], len);
    f->reads++;
    f->bytes_read += (uint32_t)len;
    return KILN_OK;
}

kiln_err_t hf_write(void *ctx, uint32_t off, const void *data, size_t len)
{
    kiln_host_flash_t *f = static_cast<kiln_host_flash_t *>(ctx);
    if ((f == nullptr) || (data == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!f->powered) {
        return KILN_ERR_IO;
    }
    if (!in_range(f, off, len)) {
        return KILN_ERR_INVALID_ARG;
    }

    f->writes++;
    if ((f->fail_write_after != 0u) && f->writes > f->fail_write_after) {
        return KILN_ERR_IO;
    }

    const uint8_t *src = static_cast<const uint8_t *>(data);

    /* NOR: a write may only clear bits.  Anything else means the caller wrote
     * over un-erased space, which on real flash yields the AND of the two and is
     * never what was intended -- so it is an error here rather than a surprise
     * later. */
    for (size_t i = 0; i < len; i++) {
        if ((f->data[off + i] & src[i]) != src[i]) {
            return KILN_ERR_STATE;
        }
    }

    /* Power cut part-way through: some bytes land, the rest do not, and the
     * medium stays that way.  A torn record, exactly as SWR-LOG-08 means it. */
    if ((f->cut_power_at_write != 0u) && f->writes == f->cut_power_at_write) {
        const size_t n = f->cut_bytes < len ? f->cut_bytes : len;
        for (size_t i = 0; i < n; i++) {
            f->data[off + i] &= src[i];
        }
        f->bytes_written += (uint32_t)n;
        f->powered = false;
        return KILN_ERR_IO;
    }

    /* Wear, as NOR actually presents it: the write is accepted, the status says
     * success, and the cell holds something else.  Modelled as stuck-at-zero
     * because that is the direction a programmed cell fails in and the only one
     * a write could produce.  Reported as KILN_OK deliberately -- a store that
     * only notices the errors it is told about does not notice this one. */
    const bool garble = f->garble_every_write ||
                        ((f->garble_write_at != 0u) && (f->writes == f->garble_write_at));
    for (size_t i = 0; i < len; i++) {
        f->data[off + i] &= garble ? 0x00u : src[i];
    }
    f->bytes_written += (uint32_t)len;
    return KILN_OK;
}

kiln_err_t hf_erase(void *ctx, uint32_t off, size_t len)
{
    kiln_host_flash_t *f = static_cast<kiln_host_flash_t *>(ctx);
    if (f == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!f->powered) {
        return KILN_ERR_IO;
    }
    if (!in_range(f, off, len)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* Erase granularity is a sector, and a caller that assumes otherwise would
     * work here and destroy a neighbour on the device. */
    if (off % f->sector_bytes != 0 || len % f->sector_bytes != 0) {
        return KILN_ERR_INVALID_ARG;
    }

    const uint32_t sectors = (uint32_t)(len / f->sector_bytes);
    if ((f->fail_erase_after != 0u) && f->erases + sectors > f->fail_erase_after) {
        return KILN_ERR_IO;
    }
    /* A sector that will not erase, which is the other half of how NOR ends:
     * the whole call fails, including a multi-sector erase that covers it, and
     * nothing in the range is touched. */
    if ((f->fail_erase_off != KILN_HOST_NO_OFFSET) &&
        (f->fail_erase_off >= off) && (f->fail_erase_off < off + len)) {
        return KILN_ERR_IO;
    }

    memset(&f->data[off], 0xFF, len);
    f->erases += sectors;
    return KILN_OK;
}

} // namespace

void kiln_host_flash_bind(kiln_host_flash_t *f, kiln_port_flash_t *out)
{
    if ((f == nullptr) || (out == nullptr)) {
        return;
    }
    out->ctx   = f;
    out->info  = hf_info;
    out->read  = hf_read;
    out->write = hf_write;
    out->erase = hf_erase;
}

/* --- key/value store --------------------------------------------------- */

void kiln_host_kv_init(kiln_host_kv_t *kv)
{
    if (kv != nullptr) {
        memset(kv, 0, sizeof(*kv));
    }
}

namespace {

kiln_host_kv_entry_t *kv_find(kiln_host_kv_t *kv, const char *ns, const char *key)
{
    for (size_t i = 0; i < KILN_HOST_KV_ENTRIES; i++) {
        if (!kv->entries[i].used) {
            continue;
        }
        if (strcmp(kv->entries[i].ns, ns) == 0 &&
            strcmp(kv->entries[i].key, key) == 0) {
            return &kv->entries[i];
        }
    }
    return NULL;
}

kiln_err_t kv_get(void *ctx, const char *ns, const char *key,
                         void *out, size_t cap, size_t *out_len)
{
    kiln_host_kv_t *kv = static_cast<kiln_host_kv_t *>(ctx);
    if ((kv == nullptr) || (ns == nullptr) || (key == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const kiln_host_kv_entry_t *e = kv_find(kv, ns, key);
    if (e == nullptr) {
        return KILN_ERR_NOT_FOUND;
    }
    if (e->len > cap) {
        return KILN_ERR_NO_SPACE;
    }

    memcpy(out, e->value, e->len);
    if (out_len != nullptr) {
        *out_len = e->len;
    }
    return KILN_OK;
}

kiln_err_t kv_set(void *ctx, const char *ns, const char *key,
                         const void *data, size_t len)
{
    kiln_host_kv_t *kv = static_cast<kiln_host_kv_t *>(ctx);
    if ((kv == nullptr) || (ns == nullptr) || (key == nullptr) || (data == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (kv->fail_writes) {
        return KILN_ERR_IO;
    }
    if (len > KILN_HOST_KV_VALUE_MAX) {
        return KILN_ERR_NO_SPACE;
    }
    if (strlen(ns) >= KILN_HOST_KV_NS_LEN || strlen(key) >= KILN_HOST_KV_KEY_LEN) {
        return KILN_ERR_INVALID_ARG;
    }

    kiln_host_kv_entry_t *e = kv_find(kv, ns, key);
    if (e == nullptr) {
        for (size_t i = 0; i < KILN_HOST_KV_ENTRIES; i++) {
            if (!kv->entries[i].used) { e = &kv->entries[i]; break; }
        }
        if (e == nullptr) {
            return KILN_ERR_NO_SPACE;
        }
        e->used = true;
        /* The strlen guards above rejected anything that would truncate; a
         * truncated namespace or key would collide two unrelated entries. */
        (void)snprintf(e->ns, sizeof(e->ns), "%s", ns);
        (void)snprintf(e->key, sizeof(e->key), "%s", key);
    }
    memcpy(e->value, data, len);
    e->len = len;
    kv->sets++;
    return KILN_OK;
}

kiln_err_t kv_erase(void *ctx, const char *ns, const char *key)
{
    kiln_host_kv_t *kv = static_cast<kiln_host_kv_t *>(ctx);
    if ((kv == nullptr) || (ns == nullptr) || (key == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    kiln_host_kv_entry_t *e = kv_find(kv, ns, key);
    if (e == nullptr) {
        return KILN_ERR_NOT_FOUND;
    }
    memset(e, 0, sizeof(*e));
    return KILN_OK;
}

kiln_err_t kv_commit(void *ctx, const char *ns)
{
    kiln_host_kv_t *kv = static_cast<kiln_host_kv_t *>(ctx);
    (void)ns;
    if (kv == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }
    if (kv->fail_writes) {
        return KILN_ERR_IO;
    }
    kv->commits++;
    return KILN_OK;
}

} // namespace

void kiln_host_kv_bind(kiln_host_kv_t *kv, kiln_port_kvstore_t *out)
{
    if ((kv == nullptr) || (out == nullptr)) {
        return;
    }
    out->ctx    = kv;
    out->get    = kv_get;
    out->set    = kv_set;
    out->erase  = kv_erase;
    out->commit = kv_commit;
}

/* --- file store -------------------------------------------------------- */

void kiln_host_fs_init(kiln_host_fs_t *fs)
{
    if (fs == nullptr) {
        return;
    }
    memset(fs, 0, sizeof(*fs));
    fs->powered = true;
}

namespace {

kiln_host_file_t *fs_find(kiln_host_fs_t *fs, const char *path)
{
    for (size_t i = 0; i < KILN_HOST_FS_FILES; i++) {
        if (fs->files[i].used && strcmp(fs->files[i].path, path) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

kiln_err_t fs_read(void *ctx, const char *path, void *out, size_t cap, size_t *len)
{
    kiln_host_fs_t *fs = static_cast<kiln_host_fs_t *>(ctx);
    if ((fs == nullptr) || (path == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!fs->powered) {
        return KILN_ERR_IO;
    }

    const kiln_host_file_t *f = fs_find(fs, path);
    if (f == nullptr) {
        return KILN_ERR_NOT_FOUND;
    }
    if (f->len > cap) {
        return KILN_ERR_NO_SPACE;
    }

    memcpy(out, f->data, f->len);
    if (len != nullptr) {
        *len = f->len;
    }
    return KILN_OK;
}

kiln_err_t fs_write_atomic(void *ctx, const char *path,
                                  const void *data, size_t len)
{
    kiln_host_fs_t *fs = static_cast<kiln_host_fs_t *>(ctx);
    if ((fs == nullptr) || (path == nullptr) || (data == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!fs->powered) {
        return KILN_ERR_IO;
    }
    if (fs->fail_writes) {
        return KILN_ERR_IO;
    }
    if (len > KILN_HOST_FS_FILE_MAX) {
        return KILN_ERR_NO_SPACE;
    }
    if (strlen(path) >= KILN_PATH_MAX) {
        return KILN_ERR_INVALID_ARG;
    }

    fs->writes++;

    /* Atomic by rename: a power cut leaves the previous content, never half of
     * the new.  Modelled by cutting power *before* anything is copied. */
    if ((fs->cut_power_at_write != 0u) && fs->writes == fs->cut_power_at_write) {
        fs->powered = false;
        return KILN_ERR_IO;
    }

    kiln_host_file_t *f = fs_find(fs, path);
    if (f == nullptr) {
        for (size_t i = 0; i < KILN_HOST_FS_FILES; i++) {
            if (!fs->files[i].used) { f = &fs->files[i]; break; }
        }
        if (f == nullptr) {
            return KILN_ERR_NO_SPACE;
        }
        f->used = true;
        /* Guarded by the strlen(path) >= KILN_PATH_MAX check above. */
        (void)snprintf(f->path, sizeof(f->path), "%s", path);
    }
    memcpy(f->data, data, len);
    f->len = len;
    return KILN_OK;
}

kiln_err_t fs_remove(void *ctx, const char *path)
{
    kiln_host_fs_t *fs = static_cast<kiln_host_fs_t *>(ctx);
    if ((fs == nullptr) || (path == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!fs->powered) {
        return KILN_ERR_IO;
    }

    kiln_host_file_t *f = fs_find(fs, path);
    if (f == nullptr) {
        return KILN_ERR_NOT_FOUND;
    }
    memset(f, 0, sizeof(*f));
    return KILN_OK;
}

kiln_err_t fs_exists(void *ctx, const char *path)
{
    kiln_host_fs_t *fs = static_cast<kiln_host_fs_t *>(ctx);
    if ((fs == nullptr) || (path == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    return (fs_find(fs, path) != nullptr) ? KILN_OK : KILN_ERR_NOT_FOUND;
}

kiln_err_t fs_list(void *ctx, const char *dir,
                          bool (*fn)(void *user, const char *name, size_t size),
                          void *user)
{
    kiln_host_fs_t *fs = static_cast<kiln_host_fs_t *>(ctx);
    if ((fs == nullptr) || (dir == nullptr) || (fn == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!fs->powered) {
        return KILN_ERR_IO;
    }

    const size_t dlen = strlen(dir);
    for (size_t i = 0; i < KILN_HOST_FS_FILES; i++) {
        if (!fs->files[i].used) {
            continue;
        }
        if (strncmp(fs->files[i].path, dir, dlen) != 0) {
            continue;
        }
        if (!fn(user, fs->files[i].path, fs->files[i].len)) {
            break;
        }
    }
    return KILN_OK;
}

kiln_err_t fs_usage(void *ctx, size_t *total, size_t *used)
{
    const kiln_host_fs_t *fs = static_cast<const kiln_host_fs_t *>(ctx);
    if (fs == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    size_t u = 0;
    for (size_t i = 0; i < KILN_HOST_FS_FILES; i++) {
        if (fs->files[i].used) {
            u += fs->files[i].len;
        }
    }
    if (total != nullptr) {
        *total = KILN_HOST_FS_FILES * KILN_HOST_FS_FILE_MAX;
    }
    if (used != nullptr) {
        *used = u;
    }
    return KILN_OK;
}

} // namespace

void kiln_host_fs_bind(kiln_host_fs_t *fs, kiln_port_filestore_t *out)
{
    if ((fs == nullptr) || (out == nullptr)) {
        return;
    }
    out->ctx          = fs;
    out->read         = fs_read;
    out->write_atomic = fs_write_atomic;
    out->remove       = fs_remove;
    out->exists       = fs_exists;
    out->list         = fs_list;
    out->usage        = fs_usage;
}

/* --- clock ------------------------------------------------------------- */

void kiln_host_clock_init(kiln_host_clock_t *c, uint64_t wall_utc_s, bool wall_valid)
{
    if (c == nullptr) {
        return;
    }
    c->mono_us    = 0;
    c->wall_utc_s = wall_utc_s;
    c->wall_valid = wall_valid;
}

void kiln_host_clock_advance(kiln_host_clock_t *c, uint64_t us)
{
    if (c == nullptr) {
        return;
    }
    c->mono_us    += us;
    c->wall_utc_s += us / 1000000u;
}

namespace {

uint64_t hc_mono(void *ctx)
{
    const kiln_host_clock_t *c = static_cast<const kiln_host_clock_t *>(ctx);
    return (c != nullptr) ? c->mono_us : 0;
}
bool hc_wall_valid(void *ctx)
{
    const kiln_host_clock_t *c = static_cast<const kiln_host_clock_t *>(ctx);
    return (c != nullptr) ? c->wall_valid : false;
}
uint64_t hc_wall(void *ctx)
{
    const kiln_host_clock_t *c = static_cast<const kiln_host_clock_t *>(ctx);
    return (c != nullptr) ? c->wall_utc_s : 0;
}

} // namespace

void kiln_host_clock_bind(kiln_host_clock_t *c, kiln_port_clock_t *out)
{
    if ((c == nullptr) || (out == nullptr)) {
        return;
    }
    out->ctx              = c;
    out->now_monotonic_us = hc_mono;
    out->wall_valid       = hc_wall_valid;
    out->now_wall_utc_s   = hc_wall;
}
