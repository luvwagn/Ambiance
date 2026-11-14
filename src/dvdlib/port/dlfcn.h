/*
 * dlfcn.h for libdvdread in amcedvd.library. libdvdread looks for
 * libdvdcss with dlopen(); here that is port.c's, which finds nothing
 * for now (an optional dvdcss.library later: see dvd-navigation.md), so
 * libdvdread reads discs without CSS.
 * Part of amcedvd.library: GPL-2.0-or-later (see src/dvdlib/README).
 */
#ifndef AMCEDVD_DLFCN_H
#define AMCEDVD_DLFCN_H
#define RTLD_LAZY 0
#define RTLD_NOW  1
void *dvdp_dlopen(const char *name, int mode);
void *dvdp_dlsym(void *handle, const char *name);
int   dvdp_dlclose(void *handle);
const char *dvdp_dlerror(void);
#define dlopen  dvdp_dlopen
#define dlsym   dvdp_dlsym
#define dlclose dvdp_dlclose
#define dlerror dvdp_dlerror
#endif
