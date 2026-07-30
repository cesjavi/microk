#ifndef UHCI_H
#define UHCI_H

/* UHCI (USB 1.1) host controller: detection and bring-up only (Etapa 0+1 of
 * the USB Mass Storage roadmap). No device enumeration, no transfers yet --
 * see ROADMAP.md for the follow-up stages (Etapa 2: control transfers via
 * Queue Heads/Transfer Descriptors, Etapa 3: Bulk-Only Transport + SCSI,
 * Etapa 4: block_device_t integration). */

int uhci_init(void);
const char *uhci_status_string(void);

#endif
