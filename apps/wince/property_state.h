#ifndef CE_PROPERTY_STATE_H
#define CE_PROPERTY_STATE_H
#include <stdbool.h>
#include <stdint.h>
#define CE_PROPERTY_PAGES 16
enum {
    CE_PSN_ACTIVE = -200,
    CE_PSN_LEAVE = -201,
    CE_PSN_APPLY = -202,
    CE_PSN_RESET = -203,
    CE_PSN_QUERY_CANCEL = -209
};
struct ce_property_state {
    unsigned count, created;
    int active;
    bool (*prepare)(void *, unsigned);
    int32_t (*notify)(void *, unsigned, int, bool);
    void (*show)(void *, unsigned, bool);
    void *context;
};
bool ce_property_select(struct ce_property_state *, unsigned);
bool ce_property_apply(struct ce_property_state *, bool);
bool ce_property_cancel(struct ce_property_state *);
#endif
