/* Named page-file mappings share bytes between views in this CE process.
 * Handles and views each retain the mapping, independently of one another. */
#include "runtime.h"
#include <glaze/thread.h>

static struct gz_mutex mapping_lock = GZ_MUTEX_INIT;
static struct ce_mapping {
    void *data;
    CW size, protect, refs;
    WCHAR name[128];
} mappings[16];
static struct {
    CW handle;
    struct ce_mapping *mapping;
} mapping_handles[32];
static struct {
    CW address;
    struct ce_mapping *mapping;
} mapping_views[32];
static CW mapping_serial;

static void mapping_release(struct ce_mapping *m)
{
    if (!--m->refs) {
        free(m->data);
        memset(m, 0, sizeof(*m));
    }
}
static CW mapping_handle(struct ce_mapping *m)
{
    for (unsigned i = 0; i < ARRAY_SIZE(mapping_handles); i++)
        if (!mapping_handles[i].handle) {
            CW h = 0x7b000000u | (++mapping_serial & 0xffffffu);
            mapping_handles[i].handle = h;
            mapping_handles[i].mapping = m;
            m->refs++;
            return h;
        }
    ce_error = 4;
    return 0;
}
CE_API(CreateFileMappingW)
{
    if (a0 != 0xffffffffu) {
        ce_error = 50; /* File-backed mappings require writeback support. */
        return 0;
    }
    if (a3 || !a4 || a4 > 4u * 1024u * 1024u || (a2 != 2 && a2 != 4) ||
        (a5 && ce_wlen(PTR(WCHAR, a5)) >= ARRAY_SIZE(mappings[0].name))) {
        ce_error = 87;
        return 0;
    }
    gz_mutex_lock(&mapping_lock);
    struct ce_mapping *available = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(mappings); i++) {
        struct ce_mapping *m = &mappings[i];
        if (!m->data) {
            available = m;
        } else if (a5 && *PTR(WCHAR, a5) && !ce_wcmp(m->name, PTR(WCHAR, a5))) {
            CW h = mapping_handle(m);
            if (h)
                ce_error = 183;
            gz_mutex_unlock(&mapping_lock);
            return h;
        }
    }
    CW result = 0;
    if (available && (available->data = calloc(1, a4))) {
        available->size = a4;
        available->protect = a2;
        if (a5)
            memcpy(available->name, PTR(void, a5), (ce_wlen(PTR(WCHAR, a5)) + 1) * 2);
        result = mapping_handle(available);
        if (!result) {
            free(available->data);
            memset(available, 0, sizeof(*available));
        } else
            ce_error = 0;
    } else
        ce_error = 8;
    gz_mutex_unlock(&mapping_lock);
    return result;
}
CE_API(MapViewOfFile)
{
    gz_mutex_lock(&mapping_lock);
    struct ce_mapping *m = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(mapping_handles); i++)
        if (a0 && mapping_handles[i].handle == a0)
            m = mapping_handles[i].mapping;
    CW result = 0;
    if (!m)
        ce_error = 6;
    else if (a2 || (a3 & 65535u) || a3 >= m->size || a4 > m->size - a3 ||
             (a1 != 2 && a1 != 4 && a1 != 6 && a1 != 0xf001fu))
        ce_error = 87;
    else if ((a1 & 2) && m->protect != 4)
        ce_error = 5;
    else {
        for (unsigned i = 0; i < ARRAY_SIZE(mapping_views); i++)
            if (!mapping_views[i].mapping) {
                result = WORD((uint8_t *)m->data + a3);
                mapping_views[i].mapping = m;
                mapping_views[i].address = result;
                m->refs++;
                break;
            }
        if (!result)
            ce_error = 8;
    }
    gz_mutex_unlock(&mapping_lock);
    return result;
}
CE_API(UnmapViewOfFile)
{
    gz_mutex_lock(&mapping_lock);
    for (unsigned i = 0; i < ARRAY_SIZE(mapping_views); i++)
        if (mapping_views[i].mapping && mapping_views[i].address == a0) {
            mapping_release(mapping_views[i].mapping);
            memset(&mapping_views[i], 0, sizeof(mapping_views[i]));
            gz_mutex_unlock(&mapping_lock);
            return 1;
        }
    gz_mutex_unlock(&mapping_lock);
    ce_error = 487;
    return 0;
}
bool ce_mapping_close(CW handle, bool *handled)
{
    *handled = (handle & 0xff000000u) == 0x7b000000u;
    if (!*handled)
        return false;
    gz_mutex_lock(&mapping_lock);
    for (unsigned i = 0; i < ARRAY_SIZE(mapping_handles); i++)
        if (mapping_handles[i].handle == handle) {
            mapping_release(mapping_handles[i].mapping);
            memset(&mapping_handles[i], 0, sizeof(mapping_handles[i]));
            gz_mutex_unlock(&mapping_lock);
            return true;
        }
    gz_mutex_unlock(&mapping_lock);
    ce_error = 6;
    return false;
}
void *ce_resolve_mapping(const char *name)
{
    static const struct ce_export exports[] = {CE_EXPORT(CreateFileMappingW), CE_EXPORT(MapViewOfFile),
                                               CE_EXPORT(UnmapViewOfFile)};
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
