#pragma once
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#define fsync _commit
/* POSIX link semantics: fail if destination exists. */
static inline int link(const char *a,const char *b) { return CreateHardLinkA(b,a,NULL) ? 0 : -1; }
static inline void installer_binary(void) { _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY); _set_fmode(_O_BINARY); }
#else
static inline void installer_binary(void) {}
#endif
