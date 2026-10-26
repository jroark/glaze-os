#ifndef STORE_WORKER_H
#define STORE_WORKER_H
#include "catalog.h"
#include <glaze/gfx.h>
#include <glaze/fs.h>
#include <glaze/pkg.h>

#define STORE_CACHE "/Library/Caches/AppStore"
enum store_job_kind {
    STORE_BOOT,
    STORE_REFRESH,
    STORE_ICON_CACHE,
    STORE_ICON,
    STORE_SCAN,
    STORE_DOWNLOAD
};
struct store_local {
    char bundle[PKG_BUNDLE_MAX];
    struct pkg_info info;
    bool installed, update;
};
struct store_icon {
    void *data;
    gfx_image img;
    bool ok, tried;
};
struct store_job {
    enum store_job_kind kind;
    unsigned generation;
    int index;
    struct store_app app;
    char root[8];
    /* Ownership passes to the worker, including when cancelled. */
    struct store_catalog *catalog;
};
struct store_result {
    enum store_job_kind kind;
    unsigned generation;
    int index, error, status, received;
    char message[96];
    struct store_catalog *catalog;
    struct store_local *local;
    void *data;
    gfx_image image;
    char package[FS_PATH_MAX];
};
bool store_worker_start(void);
/* One active job; submit cancels/replaces its predecessor without waiting. */
void store_worker_submit(const struct store_job *job);
void store_worker_cancel(void);
bool store_worker_poll(struct store_result *result, int *received);
/* Releases result buffers. A successful package file belongs to the caller,
 * which must install/remove it before submitting another download. */
void store_result_free(struct store_result *result);
/* Pure scheduling/identity helpers also exercised by host tests. */
int store_icon_next(int count, const int *priority, int npriority, const bool *done);
int store_icon_match(const struct store_catalog *old, const struct store_app *app);
void store_icons_reconcile(const struct store_catalog *old, const struct store_catalog *next,
                           struct store_icon *icons);
#endif
