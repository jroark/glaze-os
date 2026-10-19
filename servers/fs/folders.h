/* Folders every volume has: made on a fresh format (main.c) and in the
 * factory image (host/mkfsimg.c). Parents come before children. */
#ifndef FS_FOLDERS_H
#define FS_FOLDERS_H

static const char *const fs_standard_folders[] = {
    "/Documents", "/Pictures", "/Music", "/Movies", "/Library", "/Library/Preferences",
    "/Library/Notes", "/Library/Libraries",
};

#endif
