#ifndef SYSLOAD_H
#define SYSLOAD_H

/* Loads a real kernel-mode .sys driver image - see kernel/sysload.c. */
int sys_load_driver(const char *path);

#endif
