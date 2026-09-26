#ifndef WDM_DEMO_H
#define WDM_DEMO_H

/* Calls IoCreateDriver (wdm.h), which runs wdm_demo's DriverEntry and
 * registers the resulting driver_t on the "pci" devclass. Returns 0, or -1
 * if the driver pool is full or DriverEntry itself failed. Call once,
 * before bus_attach_children(pci_root()). */
int wdm_demo_init(void);

#endif
