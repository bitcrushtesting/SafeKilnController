/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware update -- SWR-UPD-01..SWR-UPD-16.
 *
 * Streamed in bounded chunks because SWR-NFR-11 leaves no room to buffer a 2 MB
 * image in RAM.  SWR-UPD-02's rollback is the adapter's job (dual OTA slots and a
 * confirm-on-boot flag); the port only has to make the two states visible, since
 * the application must confirm the running image once it has proved itself.
 *
 * SWR-UPD-04 -- refusing an update while a run or autotune is in progress -- is a
 * run-controller decision, deliberately not enforced here: the port would have
 * to know the state machine, and the check belongs where the state lives.
 *
 * The path is a PULL (OQ-08, resolved 2026-10-09): the device fetches a signed
 * static manifest and the operator at the display authorises the install.  Two
 * consequences for this interface:
 *
 *   `check` TAKES NO ARGUMENT DESCRIBING THIS DEVICE, and that is a requirement
 *   rather than an omission.  SWR-UPD-10 wants the request to carry no serial, no
 *   installed version and no query string, so the manifest is identical for every
 *   unit and the comparison happens here.  An interface that accepted "the
 *   current version" would invite a client that sends it.
 *
 *   NOTHING HERE AUTHORISES AN INSTALL.  `install` is called by the application
 *   only after a local confirmation (SWR-UPD-12, SWR-HMI-16); there is no network
 *   route to it (SWR-UPD-08), and the port deliberately offers no "install when
 *   available" convenience that could be wired to one.
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
    /* Appended rather than inserted, so the existing values keep their meaning
     * wherever a state has already been reported or logged. */
    KILN_UPD_CHECKING,     /* SWR-UPD-09: fetching and verifying the manifest  */
    KILN_UPD_AVAILABLE,    /* a verified release is newer: waiting for a human */
} kiln_upd_state_t;

typedef struct {
    kiln_upd_state_t state;
    uint32_t         received_bytes;
    uint32_t         total_bytes;    /* 0 when the client did not declare one */
    kiln_err_t       last_error;
} kiln_upd_progress_t;

/* What a verified manifest names.  `version` is the SWR-UPD-06 semantic version
 * in the same text form as kiln_fw_info_t::version, because the comparison is
 * between those two and nothing else.  `security` carries SWR-UPD-16's marking,
 * which the display shows (SWR-HMI-16) and which no logic may use to install
 * anything by itself. */
typedef struct {
    char     version[32];
    bool     security;
    uint32_t image_size;
} kiln_upd_release_t;

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

    /* SWR-UPD-09..SWR-UPD-11, SWR-UPD-13: fetch the manifest, verify its signature
     * against the key compiled into this image, and report the release it names
     * if that release is newer than the running one.  Returns KILN_ERR_NOT_FOUND
     * when there is nothing newer, which is the ordinary answer and not a fault:
     * SWR-UPD-09 makes a failed check a diagnostic.  A manifest whose signature
     * does not verify is discarded without `out` being touched. */
    kiln_err_t (*check)(void *ctx, kiln_upd_release_t *out);

    /* SWR-UPD-12: install the release `check` reported, called ONLY after a local
     * confirmation.  Streams into the inactive slot, verifies the digest from the
     * manifest and the image's own signature, then marks it bootable; the restart
     * is a separate decision made above this port. */
    kiln_err_t (*install)(void *ctx, const kiln_upd_release_t *rel);

    /* SWR-UPD-02 and SWR-UPD-15: the running image has proved itself; cancel the
     * rollback.  Call it when the supervisor link, both thermocouples, the
     * configuration, the stored programs and the display have all been seen
     * working in the running system, NOT during start-up. */
    kiln_err_t (*confirm_running)(void *ctx);
    bool       (*running_is_pending_verify)(void *ctx);
    kiln_err_t (*rollback)(void *ctx);
} kiln_port_update_t;

#endif /* KILN_PORT_UPDATE_H */
