#ifndef UHCI_H
#define UHCI_H

#include "vfs.h"

/* UHCI (USB 1.1) host controller driver. Etapa 0+1 (controller/port
 * bring-up), Etapa 2 (device enumeration via control transfers -- Queue
 * Heads/Transfer Descriptors, SET_ADDRESS/GET_DESCRIPTOR/SET_CONFIGURATION),
 * Etapa 3 (Bulk-Only Transport + minimal SCSI over the bulk endpoints Etapa 2
 * found) and Etapa 4 (block_device_t integration) of the USB Mass Storage
 * roadmap are done -- see ROADMAP.md. */

int uhci_init(void);
const char *uhci_status_string(void);

/* Runs INQUIRY/TEST UNIT READY/READ CAPACITY(10)/READ(10) against the
 * attached mass-storage device (if any) and formats the result.
 * with_write_test additionally exercises WRITE(10) with a roundtrip check
 * at a scratch LBA -- only pass 1 against a disposable test image, never
 * against real hardware. */
const char *uhci_msd_test_string(int with_write_test);

/* Probes the attached mass-storage device with TEST UNIT READY/READ
 * CAPACITY(10) and, on success, prepares the block_device_t returned by
 * uhci_msd_primary(). Returns 0 on success, -1 if there's no usable
 * mass-storage device (none attached, not configured, or it didn't answer
 * READ CAPACITY(10)). Call once after uhci_init(); mirrors ata_init() +
 * ata_primary_master(). */
int uhci_msd_init(void);
block_device_t *uhci_msd_primary(void);

#endif
