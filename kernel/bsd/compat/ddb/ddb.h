#ifndef GENESIS_BSD_COMPAT_DDB_DDB_H
#define GENESIS_BSD_COMPAT_DDB_DDB_H
/* The in-kernel debugger, compiled out. DB_SHOW_COMMAND defines a function
 * nothing calls; declaring it static keeps -Wunused-function quiet without
 * deleting the vendored code. */
#define DB_SHOW_COMMAND(name, fn) static void __unused fn(void)
#define DB_SHOW_COMMAND_FLAGS(name, fn, fl) static void __unused fn(void)
#define db_printf(...)  do { } while (0)
#define db_pager_quit   0
#endif
