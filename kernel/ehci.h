#ifndef EHCI_H
#define EHCI_H

/* EHCI (USB 2.0) host controller driver. Etapa 0+1 of the "USB moderno"
 * roadmap (ROADMAP.md, "Etapa 3: Controlador USB moderno minimo"):
 * PCI detection, BIOS handoff, controller reset/bring-up, and root port
 * reset+enable -- same scope as kernel/uhci.c's own Etapa 0+1, no
 * enumeration/transfers yet (that's a later stage, reusing the
 * Bulk-Only-Transport/SCSI code already proven there once EHCI can run
 * control/bulk transfers). */

int ehci_init(void);
const char *ehci_status_string(void);

#endif
