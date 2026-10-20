/*
 * App packages. A package (.gzp) is one file holding an app: a romfs-style
 * archive with the magic "GZPK" (tools/mkpkg.py):
 *
 *   header:  "GZPK" u32 count
 *   entries: char name[48], u32 offset, u32 size   (offset from file start)
 *
 * Its entries are "Info" (key=value lines: id, name, version, exec), the
 * executable named by exec (default "app", a PIE linked like built-in
 * apps), and resources under "res/" ("res/icon.gzi" is the home-screen
 * icon). Installing unpacks them into a bundle folder,
 * <root>/Applications/<id>.app, on the flash (root "") or the card
 * ("/Card"). The bundle path is the app's identity for SpringBoard; an app
 * keeps its own data under <bundle>/data, which updates preserve and
 * uninstalling removes. Packages cannot supply entries in data/.
 */
#ifndef GLAZE_PKG_H
#define GLAZE_PKG_H

#include <glaze/types.h>

#define PKG_ID_MAX      24
#define PKG_BUNDLE_MAX  48      /* "/Card/Applications/" + id + ".app" + NUL */

struct pkg_info {
    char id[PKG_ID_MAX + 1];    /* [a-z0-9.-] */
    char name[32];              /* home-screen label */
    char version[16];
    char exec[16];
};

/* The Info of a package file, or of an installed bundle. */
int pkg_read_info(const char *gzp, struct pkg_info *info);
int pkg_bundle_info(const char *bundle, struct pkg_info *info);
/* One entry of a package file (such as "res/icon.gzi"): its size, or an
 * error (GZ_EFULL if it is larger than max). */
int pkg_read_entry(const char *gzp, const char *name, void *buf, uint32_t max);
/* Installs a package under root ("" or "/Card"), preserving saved data
 * and recovering interrupted replacements. Replaces an installed app
 * with the same id on either volume. Writes the bundle path to `bundle`. */
int pkg_install(const char *gzp, const char *root, char bundle[PKG_BUNDLE_MAX]);
int pkg_uninstall(const char *bundle);
/* The installed bundle of an id on either volume, or GZ_ENOENT. */
int pkg_find(const char *id, char bundle[PKG_BUNDLE_MAX]);
/* Calls fn for each app installed under root, in name order; returns the
 * count. Interrupted installs found on the way are removed. */
int pkg_list(const char *root, void (*fn)(const char *bundle, const struct pkg_info *info, void *arg), void *arg);

#endif
