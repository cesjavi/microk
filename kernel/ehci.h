#ifndef EHCI_H
#define EHCI_H

#include "vfs.h"

/* EHCI (USB 2.0) host controller driver. Etapa 0+1 (controller/port
 * bring-up), Etapa 2 (device enumeration via control transfers) and
 * Etapa 3+4 (Bulk-Only Transport + minimal SCSI + block_device_t
 * integration) of the "USB moderno" roadmap (ROADMAP.md, "Etapa 3:
 * Controlador USB moderno minimo") are done -- mirrors kernel/uhci.c's
 * own staged history. */

int ehci_init(void);
const char *ehci_status_string(void);

/* Runs INQUIRY/TEST UNIT READY/READ CAPACITY(10)/READ(10) against the
 * attached mass-storage device (if any) and formats the result. Mirrors
 * kernel/uhci.c's uhci_msd_test_string. with_write_test additionally
 * exercises WRITE(10) with a roundtrip check at a scratch LBA -- only
 * pass 1 against a disposable test image, never real hardware. */
const char *ehci_msd_test_string(int with_write_test);

/* Probes the attached mass-storage device with TEST UNIT READY/READ
 * CAPACITY(10) and, on success, prepares the block_device_t returned by
 * ehci_msd_primary(). Returns 0 on success, -1 if there's no usable
 * mass-storage device. Call once after ehci_init(); mirrors
 * kernel/uhci.c's uhci_msd_init()/uhci_msd_primary(). */
int ehci_msd_init(void);
block_device_t *ehci_msd_primary(void);

#endif
