/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port.h"
#include "doomkeys.h"
#include <glaze/abi.h>
int doom_map_key(unsigned code, int in_menu)
{
    switch (code) {
    case GZ_KEY_UP: return KEY_UPARROW;
    case GZ_KEY_DOWN: return KEY_DOWNARROW;
    case GZ_KEY_LEFT: return KEY_LEFTARROW;
    case GZ_KEY_RIGHT: return KEY_RIGHTARROW;
    case GZ_KEY_OK: return in_menu ? KEY_ENTER : KEY_FIRE;
    case GZ_KEY_ESC: return in_menu ? KEY_ESCAPE : KEY_USE;
    case GZ_KEY_ENTER: return KEY_ENTER;
    case GZ_KEY_TAB: return KEY_TAB;
    case GZ_KEY_BACKSPACE: return KEY_BACKSPACE;
    case GZ_KEY_DELETE: return KEY_DEL;
    default: return 0;
    }
}
