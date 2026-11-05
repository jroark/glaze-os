/* Dial-up configuration checks (glaze/net.h), shared by the client and the
 * network server; built natively by tests/native/modem_chat.c. */
#include <glaze/net.h>
#include <stdbool.h>
#include <string.h>

static bool terminated(const char *s, unsigned size, bool printable)
{
    const char *end = memchr(s, 0, size);
    if (!end) return false;
    for (const char *p = s; printable && p < end; p++)
        if (*p < 32 || *p > 126) return false;
    return true;
}

int net_dial_config_validate(const struct net_dial_config *c)
{
    if (!c || !terminated(c->number, sizeof(c->number), true) ||
        !terminated(c->user, sizeof(c->user), true) ||
        !terminated(c->password, sizeof(c->password), true) ||
        !terminated(c->init, sizeof(c->init), true))
        return GZ_EINVAL;
    /* Nothing that could end the dial command and start another one. */
    for (const char *p = c->number; *p; p++)
        if (!strchr("0123456789,Ww*#PpTt()- ", *p)) return GZ_EINVAL;
    if (strchr(c->init, ';')) return GZ_EINVAL;
    return 0;
}
