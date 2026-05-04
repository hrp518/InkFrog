#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

/* XR872 (ARM Cortex-M, little endian) */
#define BYTEORDER 1234

/* no POSIX system calls on FreeRTOS - use undef comment style */
/* #undef HAVE_ARC4RANDOM */
/* #undef HAVE_ARC4RANDOM_BUF */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_FCNTL_H */
/* #undef HAVE_GETENTROPY */
/* #undef HAVE_GETPAGESIZE */
/* #undef HAVE_GETRANDOM */
/* #undef HAVE_MMAP */
/* #undef HAVE_SYSCALL_GETRANDOM */
/* #undef HAVE_STRINGS_H */
/* #undef HAVE_SYS_PARAM_H */
/* #undef HAVE_SYS_STAT_H */
/* #undef HAVE_SYS_TYPES_H */
/* #undef HAVE_UNISTD_H */

/* standard C headers available on XR872 */
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
/* #undef HAVE_INTTYPES_H */

/* package info */
#define PACKAGE "expat"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "expat"
#define PACKAGE_STRING "expat 2.8.0"
#define PACKAGE_TARNAME "expat"
#define PACKAGE_URL ""
#define PACKAGE_VERSION "2.8.0"
#define VERSION "2.8.0"

#define STDC_HEADERS 1

/* XML feature flags - minimal config for XHTML parsing */
#define XML_CONTEXT_BYTES 0
/* #undef XML_DEV_URANDOM */
/* #undef XML_DTD */
#define XML_GE 0
/* #undef XML_NS */
#define XML_POOR_ENTROPY 1

#endif /* EXPAT_CONFIG_H */
