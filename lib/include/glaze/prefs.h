/*
 * Preferences: small settings values kept as files in the file system, one
 * file per key under PREFS_DIR. Keys are plain names ("sb.wallpaper").
 */
#ifndef GLAZE_PREFS_H
#define GLAZE_PREFS_H

#include <glaze/types.h>

#define PREFS_DIR   "/Library/Preferences"

/* Bytes read (up to len), or an error (GZ_ENOENT if the key is unset). */
int pref_get(const char *key, void *buf, uint32_t len);
/* Replaces the value; creates PREFS_DIR if it is missing. */
int pref_put(const char *key, const void *buf, uint32_t len);
int pref_delete(const char *key);

#endif
