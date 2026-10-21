/* Property-page validation belongs to the original DLGPROC. A TRUE callback
 * return means 'handled'; DWL_MSGRESULT supplies the veto/error value. */
#include "property_state.h"
bool ce_property_select(struct ce_property_state *s, unsigned index)
{
    if (!s || !s->count || s->count > CE_PROPERTY_PAGES || index >= s->count)
        return false;
    if (s->active == (int)index)
        return true;
    if (s->active >= 0 && s->notify(s->context, s->active, CE_PSN_LEAVE, false))
        return false;
    if (!(s->created & (1u << index))) {
        if (!s->prepare(s->context, index))
            return false;
        s->created |= 1u << index;
    }
    if (s->notify(s->context, index, CE_PSN_ACTIVE, false))
        return false;
    if (s->active >= 0)
        s->show(s->context, s->active, false);
    s->active = index;
    s->show(s->context, index, true);
    return true;
}
bool ce_property_apply(struct ce_property_state *s, bool closing)
{
    if (!s || s->active < 0 || s->notify(s->context, s->active, CE_PSN_LEAVE, false))
        return false;
    for (unsigned i = 0; i < s->count; i++)
        if (s->created & (1u << i)) {
            int32_t result = s->notify(s->context, i, CE_PSN_APPLY, closing);
            if (result) {
                if (result == 1 && s->active != (int)i) {
                    s->show(s->context, s->active, false);
                    s->active = i;
                    s->show(s->context, i, true);
                }
                return false;
            }
        }
    return true;
}
bool ce_property_cancel(struct ce_property_state *s)
{
    if (!s || s->active < 0 || s->notify(s->context, s->active, CE_PSN_QUERY_CANCEL, false))
        return false;
    for (unsigned i = 0; i < s->count; i++)
        if (s->created & (1u << i))
            s->notify(s->context, i, CE_PSN_RESET, false);
    return true;
}
