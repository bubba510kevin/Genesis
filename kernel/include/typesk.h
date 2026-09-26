#ifndef TYPES_H
#define TYPES_H

typedef unsigned char      uint8;
typedef signed char        int8;
typedef unsigned short      uint16;
typedef signed short        int16;
typedef unsigned int        uint32;
typedef signed int          int32;
typedef unsigned long long  uint64;
typedef signed long long    int64;


typedef uint8 uint8_t; 
typedef int8 int8_t ;
typedef uint16 uint16_t;
typedef int16 int16_t;
typedef uint32 uint32_t;
typedef int32 int32_t;
typedef uint64 uint64_t;
typedef int64 int64_t;
typedef unsigned long u_long;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef long long quad_t;
typedef unsigned long long u_quad_t;

/* --- address and size types ------------------------------------------------
 * Everything above is a fixed-width type: use those when the width is part of
 * the contract (hardware registers, on-disk structures, page table entries of
 * a KNOWN format). Use the types below whenever the value is an address, an
 * object size, or a count of bytes - those follow the target and are the ones
 * that make a 64-bit port mechanical instead of a rewrite.
 *
 *   uintptr      - integer wide enough to hold any pointer
 *   virt_addr_t  - a virtual address
 *   phys_addr_t  - a physical address. Separate from virt_addr_t on purpose:
 *                  they are not interchangeable, and letting the compiler say
 *                  so catches the classic "passed a virtual address to the
 *                  MMU" bug at build time rather than as a triple fault.
 *   size_t       - a byte count
 *
 * Note phys_addr_t and virt_addr_t are the same underlying width today, so the
 * compiler will NOT actually catch a mix-up yet - they are documentation until
 * you make them distinct struct types. Naming them correctly now is still what
 * lets the long-mode port change these four lines instead of every signature. */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__)
typedef uint64 uintptr;
typedef uint64 phys_addr_t;
#else
typedef uint32 uintptr;
typedef uint32 phys_addr_t;   /* widen to uint64 if you enable PAE in 32-bit */
#endif

typedef uintptr virt_addr_t;
typedef uintptr size_t;

/* Guarded, because this is not the only header in the build that defines it:
 * GCC's own freestanding <stddef.h> does too, and a translation unit that
 * reaches that first (the driver-compat sources do, through sys/bus.h) gets a
 * redefinition warning for a macro both spellings agree on. */
#ifndef NULL
#define NULL ((void*)0)
#endif

#endif