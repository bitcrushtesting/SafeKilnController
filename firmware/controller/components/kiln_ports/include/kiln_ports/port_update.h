/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware update -- SWR-UPD-01..SWR-UPD-05, SWR-UPD-07.
 *
 * Streamed in bounded chunks because SWR-NFR-11 leaves no room to buffer a 2 MB
 * image in RAM.  SWR-UPD-02's rollback is the adapter's job (dual OTA slots and a
 * confirm-on-boot flag); the port only has to make the two states visible, since
 * the application must confirm the running image once it has proved itself.
 *
 * SWR-UPD-04 -- refusing an update while a run or autotune is in progress -- is a
 * run-controller decision, deliberately not enforced here: the port would have
 * to know the state machine, and the check belongs where the state lives.
 */
#ifndef KILN_PORT_UPDATE_H
#define KILN_PORT_UPDATE_H

#include <stddef.h>
#include "kiln/err.h"
#include "kiln/types.h"

typedef enum {
    KILN_UPD_IDLE = 0,
    KILN_UPD_RECEIVING,
    KILN_UPD_VERIFYING,
    KILN_UPD_READY,        /* verified and marked bootable: restart to apply */
    KILN_UPD_FAILED,
} kiln_upd_state_t;

typedef struct {
    kiln_upd_state_t state;
    uint32_t         received_bytes;
    uint32_t         total_bytes;    /* 0 when the client did not declare one */
    kiln_err_t       last_error;
} kiln_upd_progress_t;

typedef struct kiln_port_update {
    void *ctx;
    /* total_bytes may be 0 for a chunked upload. */
    kiln_err_t (*begin)(void *ctx, uint32_t total_bytes);
    kiln_err_t (*write)(void *ctx, const void *data, size_t len);
    /* SWR-UPD-03: verify, then mark bootable.  Rejects an image that is not a
     * valid application for this target. */
    kiln_err_t (*finish)(void *ctx);
    void       (*abort)(void *ctx);
    kiln_err_t (*progress)(void *ctx, kiln_upd_progress_t *out);

    /* SWR-UPD-02: the running image has proved itself; cancel the rollback. */
    kiln_err_t (*confirm_running)(void *ctx);
    bool       (*running_is_pending_verify)(void *ctx);
    kiln_err_t (*rollback)(void *ctx);
} kiln_port_update_t;

#endif /* KILN_PORT_UPDATE_H */
