#ifndef STORE_CATALOG_H
#define STORE_CATALOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define STORE_MAX_APPS 32
#define STORE_CATALOG_MAX (64 * 1024)
#define STORE_PACKAGE_MAX (2 * 1024 * 1024)
#define STORE_ICON_MAX 8192
struct store_app {
    char id[25], name[32], version[16], developer[48];
    char summary[80], description[512];
    char package[128], icon[128], sha256[65], icon_sha256[65];
    uint32_t size;
};
struct store_catalog {
    int count;
    struct store_app apps[STORE_MAX_APPS];
};
bool store_version_ok(const char *s);
int store_version_cmp(const char *a, const char *b);
bool store_path_ok(const char *s);
bool store_id_ok(const char *s);
/* Rejects the whole catalog on malformed, duplicate, missing or excessive data. */
bool store_parse(struct store_catalog *out, const char *data, size_t len);
bool store_matches(const struct store_app *app, const char *query);
#endif
