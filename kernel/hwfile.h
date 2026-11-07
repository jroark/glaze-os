/* Driver, firmware and service files from romfs or installed hardware
 * packages (glaze/hwpkg.h, docs/os/HARDWARE_PACKAGES.md). */
#ifndef GLAZE_KERNEL_HWFILE_H
#define GLAZE_KERNEL_HWFILE_H
#include "proc.h"

struct hwfile {
    const struct romfs_entry *rom;  /* a romfs file, or */
    const uint8_t *buf;             /* a verified package file in kernel pages */
    uint32_t size;
    int slot;
};

/* romfs first, then the installed packages. 0 and an open file; GZ_EINPROGRESS
 * while the bytes are being fetched (try again later: never wait for them);
 * GZ_ENOENT if no one has it; GZ_ECORRUPT if the installed copy didn't match
 * its manifest; GZ_ENOMEM. */
int hwfile_open(const char *path, struct hwfile *f);
/* The same for an overridable service: the installed package first, and
 * GZ_ENOENT (use the romfs copy) if none supplies it. */
int hwfile_open_package(const char *path, struct hwfile *f);
int hwfile_read(const struct hwfile *f, uint32_t off, void *dst, uint32_t len);
void hwfile_close(struct hwfile *f);
/* Frees fetched files no one holds open. Call it once a consumer is done
 * (it may free what another is collecting, which then fetches it again). */
void hwfile_trim(void);
/* Bumped by each registration; a missing driver is worth another look. */
uint32_t hwfile_generation(void);
int sys_hw_ctl(struct proc *p, uint32_t op, uint32_t a1, uint32_t a2);

#ifdef HWFILE_TEST
/* host/hwfile_test.c verifies with this key instead of the release key. */
extern uint8_t hwfile_test_key[32];
#endif
#endif
