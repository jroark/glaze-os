#include "worker.h"
#include <string.h>
#include <stdlib.h>

int store_icon_next(int count, const int *priority, int npriority, const bool *done)
{
    for (int i = 0; i < npriority; i++)
        if (priority[i] >= 0 && priority[i] < count && !done[priority[i]])
            return priority[i];
    for (int i = 0; i < count; i++)
        if (!done[i])
            return i;
    return -1;
}
int store_icon_match(const struct store_catalog *old, const struct store_app *app)
{
    for (int i = 0; i < old->count; i++)
        if (!strcmp(old->apps[i].id, app->id) &&
            !strcmp(old->apps[i].icon_sha256, app->icon_sha256))
            return i;
    return -1;
}

void store_icons_reconcile(const struct store_catalog *old, const struct store_catalog *next,
                           struct store_icon *icons)
{
    struct store_icon kept[STORE_MAX_APPS] = {0};
    for (int i = 0; i < next->count; i++) {
        int match = store_icon_match(old, &next->apps[i]);
        if (match >= 0) {
            kept[i] = icons[match];
            memset(&icons[match], 0, sizeof(icons[match]));
        }
        kept[i].tried = false;
    }
    for (int i = 0; i < STORE_MAX_APPS; i++)
        free(icons[i].data);
    memcpy(icons, kept, sizeof(kept));
}
