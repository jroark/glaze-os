/*
 * The device passcode SpringBoard asks for after slide to unlock. Kept as
 * the "sb.passcode" preference: a salt and an iterated hash, never the
 * digits. A 4-digit code is no protection against a process that can read
 * the preference and try all 10,000; it keeps the code itself off the flash.
 */
#ifndef GLAZE_PASSCODE_H
#define GLAZE_PASSCODE_H

#include <glaze/types.h>

#define PASSCODE_PREF   "sb.passcode"

bool passcode_enabled(void);
bool passcode_check(const char *code);
/* Sets the passcode, or turns it off when code is NULL. */
int passcode_set(const char *code);

#endif
