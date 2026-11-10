/* CE remote access (RAS) and the TAPI calls the Connections and Dial-up
 * applets make, on Glaze's dial-up (the network server's CF modem and PPP,
 * glaze/net.h). The phonebook is CE's, HKCU\Comm\RasBook\<entry> (Entry:
 * a RASENTRY, DevCfg) in the shared registry, which starts as the device's
 * own; dial parameters are stored beside it (User, Password, Domain).
 * Without a modem Glaze is online through its network card or USB, and a
 * dial reports that connection, so applets that dial first still work.
 * RAS: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms897133(v=msdn.10)
 * TAPI: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms895571(v=msdn.10) */
#include "runtime.h"
#include <glaze/net.h>
#include <glaze/thread.h>

#define RASBOOK "HKCU\\Comm\\RasBook"
#define NAME_UNITS 21       /* RAS_MaxEntryName + 1 */
#define DEVICE_TYPE_UNITS 17
#define DEVICE_NAME_UNITS 33
#define RAS_HANDLE 0x7e000001u
enum { RASCS_OPENPORT = 0, RASCS_CONNECTDEVICE = 2, RASCS_DEVICECONNECTED = 3, RASCS_AUTHENTICATE = 5,
       RASCS_AUTHENTICATED = 14, RASCS_CONNECTED = 0x2000, RASCS_DISCONNECTED = 0x2001 };
enum { WM_RASDIALEVENT = 0xcccd };

static bool modem(void)
{
    struct net_status st;
    return !net_status(&st) && st.kind == GZ_NET_MODEM;
}
static void entry_key(char out[160], const WCHAR *name)
{
    char narrow[64];
    ce_narrow(narrow, sizeof(narrow), name);
    snprintf(out, 160, RASBOOK "\\%s", narrow);
}
static void put_units(uint8_t *dst, unsigned units, const char *text)
{
    for (unsigned i = 0; i < units; i++) {
        WCHAR c = i + 1 < units && text[i] ? (uint8_t)text[i] : 0;
        ce_put16(dst + i * 2, c);
        if (!c)
            break;
    }
}

/* RasEnumEntries(reserved, phonebook, RASENTRYNAME[], bytes, count):
 * RASENTRYNAME is {dwSize, WCHAR name[21]}, 48 bytes with padding. */
CE_API(RasEnumEntries)
{
    CW *bytes = PTR(CW, a3), *count = PTR(CW, a4);
    if (!bytes || !count)
        return 87;
    char name[64];
    unsigned n = 0;
    while (ce_registry_child(RASBOOK, n, name, sizeof(name)))
        n++;
    CW need = n * 48u, have = *bytes;
    *bytes = need;
    *count = n;
    if (!a2 || have < need)
        return n ? 603 : 0; /* ERROR_BUFFER_TOO_SMALL */
    for (unsigned i = 0; i < n; i++) {
        uint8_t *e = PTR(uint8_t, a2) + i * 48;
        memset(e, 0, 48);
        ce_put32(e, 48);
        if (ce_registry_child(RASBOOK, i, name, sizeof(name)))
            put_units(e + 4, NAME_UNITS, name);
    }
    return 0;
}
/* RasGetEntryProperties(phonebook, entry, RASENTRY, size, device info, size).
 * An empty entry name asks for a default RASENTRY. */
CE_API(RasGetEntryProperties)
{
    CW *size = PTR(CW, a3), *device_size = PTR(CW, a5);
    if (!size)
        return 87;
    char key[160];
    uint8_t entry[1024], device[256];
    int entry_bytes = -1, device_bytes = 0;
    if (a1 && *PTR(WCHAR, a1)) {
        entry_key(key, PTR(WCHAR, a1));
        entry_bytes = ce_registry_get(key, "Entry", NULL, entry, sizeof(entry));
        if (entry_bytes < 0)
            return 623; /* ERROR_CANNOT_FIND_PHONEBOOK_ENTRY */
        device_bytes = MAX(0, ce_registry_get(key, "DevCfg", NULL, device, sizeof(device)));
    } else {
        memset(entry, 0, sizeof(entry));
        entry_bytes = a2 && *size >= 4 ? (int)MIN(ce_u32(PTR(uint8_t, a2)), (CW)sizeof(entry)) : 4;
        ce_put32(entry, (CW)entry_bytes);
    }
    entry_bytes = MIN(entry_bytes, (int)sizeof(entry));
    CW have = *size;
    *size = (CW)entry_bytes;
    if (device_size) {
        CW device_have = *device_size;
        *device_size = (CW)device_bytes;
        if (a4 && device_have >= (CW)device_bytes)
            memcpy(PTR(void, a4), device, (size_t)device_bytes);
    }
    if (!a2 || have < (CW)entry_bytes)
        return 603;
    memcpy(PTR(void, a2), entry, (size_t)entry_bytes);
    return 0;
}
CE_API(RasSetEntryProperties)
{
    if (!a1 || !*PTR(WCHAR, a1) || !a2 || a3 < 4 || a3 > 1024)
        return 87;
    char key[160];
    entry_key(key, PTR(WCHAR, a1));
    if (!ce_registry_put(key, "Entry", 3, PTR(void, a2), a3))
        return 5;
    if (a4 && a5 && a5 <= 256)
        ce_registry_put(key, "DevCfg", 3, PTR(void, a4), a5);
    return 0;
}

/* RASDIALPARAMS: dwSize, szEntryName[21], szPhoneNumber[129],
 * szCallbackNumber[49], szUserName[257], szPassword[257], szDomain[16]. */
#define DP_ENTRY 4
#define DP_PHONE (DP_ENTRY + NAME_UNITS * 2)
#define DP_CALLBACK (DP_PHONE + 129 * 2)
#define DP_USER (DP_CALLBACK + 49 * 2)
#define DP_PASSWORD (DP_USER + 257 * 2)
#define DP_DOMAIN (DP_PASSWORD + 257 * 2)
#define DP_SIZE (DP_DOMAIN + 16 * 2)
static void get_string(const char *key, const char *name, uint8_t *dst, unsigned units)
{
    WCHAR text[257] = {0};
    ce_registry_string(key, name, text, MIN(units, (unsigned)ARRAY_SIZE(text)));
    memcpy(dst, text, (ce_wlen(text) + 1) * 2);
}
static void put_string(const char *key, const char *name, const uint8_t *src, unsigned units)
{
    WCHAR text[257];
    memcpy(text, src, MIN(units, 257u) * 2);
    text[MIN(units, 257u) - 1] = 0;
    ce_registry_put(key, name, 1, text, (ce_wlen(text) + 1) * 2);
}
/* RasGetEntryDialParams(phonebook, RASDIALPARAMS, has password). */
CE_API(RasGetEntryDialParams)
{
    uint8_t *p = PTR(uint8_t, a1);
    if (!p || ce_u32(p) < DP_SIZE)
        return 632; /* ERROR_INVALID_SIZE */
    char key[160];
    entry_key(key, (const WCHAR *)(p + DP_ENTRY));
    uint8_t probe[4];
    if (ce_registry_get(key, "Entry", NULL, probe, 0) < 0)
        return 623;
    get_string(key, "User", p + DP_USER, 257);
    get_string(key, "Password", p + DP_PASSWORD, 257);
    get_string(key, "Domain", p + DP_DOMAIN, 16);
    memset(p + DP_PHONE, 0, 129 * 2);
    memset(p + DP_CALLBACK, 0, 49 * 2);
    if (a2)
        *PTR(CW, a2) = ce_u16(p + DP_PASSWORD) != 0;
    return 0;
}
/* RasSetEntryDialParams(phonebook, RASDIALPARAMS, remove password). */
CE_API(RasSetEntryDialParams)
{
    uint8_t *p = PTR(uint8_t, a1);
    if (!p || ce_u32(p) < DP_SIZE)
        return 632;
    char key[160];
    entry_key(key, (const WCHAR *)(p + DP_ENTRY));
    put_string(key, "User", p + DP_USER, 257);
    put_string(key, "Domain", p + DP_DOMAIN, 16);
    static const WCHAR empty[1];
    put_string(key, "Password", a2 ? (const uint8_t *)empty : p + DP_PASSWORD, a2 ? 1 : 257);
    return 0;
}

/* One connection at a time. RasDial(extensions, phonebook, RASDIALPARAMS,
 * notifier type, notifier, handle): a notifier type of 0xffffffff names a
 * window for WM_RASDIALEVENT (state, error); otherwise the call is
 * synchronous. */
static struct {
    bool active, modem;
    CW window;
    unsigned state;
    CW error;
} conn;
static unsigned dial_state(const struct net_dial_status *st)
{
    switch (st->state) {
    case NET_DIAL_STARTING: return RASCS_OPENPORT;
    case NET_DIAL_DIALING: return RASCS_CONNECTDEVICE;
    case NET_DIAL_NEGOTIATING: return RASCS_AUTHENTICATE;
    case NET_DIAL_UP: return RASCS_CONNECTED;
    default: return RASCS_DISCONNECTED;
    }
}
static CW dial_error(int32_t e)
{
    static const CW map[] = {0, 692, 692, 680, 676, 678, 619, 678, 691, 734, 692};
    return e >= 0 && (unsigned)e < ARRAY_SIZE(map) ? map[e] : 692; /* ERROR_HARDWARE_FAILURE */
}
static void notify(unsigned state, CW error)
{
    conn.state = state;
    conn.error = error;
    if (conn.window)
        ce_post(conn.window, WM_RASDIALEVENT, state, error);
}
static void watch(void *arg)
{
    (void)arg;
    unsigned last = RASCS_OPENPORT;
    for (uint32_t waited = 0; conn.active && waited < 120000; waited += 500) {
        struct net_dial_status st;
        if (net_dial_status(&st))
            break;
        unsigned state = dial_state(&st);
        if (state != last && state != RASCS_DISCONNECTED)
            notify(state, 0);
        last = state;
        if (state == RASCS_CONNECTED)
            return;
        if (st.state == NET_DIAL_IDLE && waited > 1000) {
            notify(RASCS_DISCONNECTED, dial_error(st.error));
            conn.active = false;
            return;
        }
        gz_sleep_ms(500);
    }
    if (conn.active && conn.state != RASCS_CONNECTED) {
        notify(RASCS_DISCONNECTED, 678);
        conn.active = false;
    }
}
CE_API(RasDial)
{
    const uint8_t *p = PTR(uint8_t, a2);
    CW *handle = PTR(CW, a5);
    if (!p || !handle || ce_u32(p) < DP_SIZE)
        return 87;
    if (conn.active)
        return 602; /* ERROR_PORT_ALREADY_OPEN */
    conn = (typeof(conn)){true, modem(), a3 == 0xffffffffu ? a4 : 0, RASCS_OPENPORT, 0};
    *handle = RAS_HANDLE;
    if (!conn.modem) {
        /* Already online through the network card or USB. */
        bool up = !net_wait_up(0);
        notify(up ? RASCS_CONNECTED : RASCS_DISCONNECTED, up ? 0 : 619);
        conn.active = up;
        return up || conn.window ? 0 : 619; /* ERROR_PORT_DISCONNECTED */
    }
    struct net_dial_config config = {0};
    char key[160];
    entry_key(key, (const WCHAR *)(p + DP_ENTRY));
    /* The number: the parameters' own, else the entry's area code and
     * local number (RASENTRY offsets 16 and 38). */
    ce_narrow(config.number, sizeof(config.number), (const WCHAR *)(p + DP_PHONE));
    if (!config.number[0]) {
        uint8_t entry[1024];
        if (ce_registry_get(key, "Entry", NULL, entry, sizeof(entry)) >= 296) {
            char area[12], local[NET_DIAL_NUMBER_MAX];
            ce_narrow(area, sizeof(area), (const WCHAR *)(entry + 16));
            ce_narrow(local, sizeof(local), (const WCHAR *)(entry + 38));
            snprintf(config.number, sizeof(config.number), "%s%s", area, local);
        }
    }
    ce_narrow(config.user, sizeof(config.user), (const WCHAR *)(p + DP_USER));
    ce_narrow(config.password, sizeof(config.password), (const WCHAR *)(p + DP_PASSWORD));
    config.keep_password = !config.password[0];
    int rv = net_dial(&config);
    if (rv) {
        conn.active = false;
        notify(RASCS_DISCONNECTED, 692);
        return 692;
    }
    notify(RASCS_OPENPORT, 0);
    if (conn.window)
        return gz_thread_create(watch, NULL) < 0 ? 8 : 0;
    watch(NULL);
    return conn.state == RASCS_CONNECTED ? 0 : conn.error;
}
CE_API(RasHangup)
{
    if (a0 != RAS_HANDLE)
        return 6; /* ERROR_INVALID_HANDLE */
    if (conn.modem)
        net_hangup();
    conn.active = false;
    conn.state = RASCS_DISCONNECTED;
    return 0;
}
/* RASCONNSTATUS: dwSize, state, error, szDeviceType[17], szDeviceName[33]. */
CE_API(RasGetConnectStatus)
{
    uint8_t *s = PTR(uint8_t, a1);
    if (a0 != RAS_HANDLE)
        return 6;
    if (!s || ce_u32(s) < 12 + (DEVICE_TYPE_UNITS + DEVICE_NAME_UNITS) * 2)
        return 632;
    if (conn.modem && conn.active) {
        struct net_dial_status st;
        if (!net_dial_status(&st))
            conn.state = dial_state(&st);
    }
    ce_put32(s + 4, conn.state);
    ce_put32(s + 8, conn.error);
    put_units(s + 12, DEVICE_TYPE_UNITS, conn.modem ? "modem" : "direct");
    put_units(s + 12 + DEVICE_TYPE_UNITS * 2, DEVICE_NAME_UNITS, conn.modem ? "Glaze CF Modem" : "Glaze Network");
    return 0;
}
/* RASDEVINFO: dwSize, szDeviceType[17], szDeviceName[33] (104 bytes). */
CE_API(RasEnumDevicesW)
{
    CW *bytes = PTR(CW, a1), *count = PTR(CW, a2);
    const CW size = 4 + (DEVICE_TYPE_UNITS + DEVICE_NAME_UNITS) * 2;
    if (!bytes || !count)
        return 87;
    bool m = modem();
    CW have = *bytes;
    *bytes = m ? size : 0;
    *count = m ? 1 : 0;
    if (!m)
        return 0;
    if (!a0 || have < size)
        return 603;
    uint8_t *d = PTR(uint8_t, a0);
    ce_put32(d, size);
    put_units(d + 4, DEVICE_TYPE_UNITS, "modem");
    put_units(d + 4 + DEVICE_TYPE_UNITS * 2, DEVICE_NAME_UNITS, "Glaze CF Modem");
    return 0;
}

/* TAPI: one line, the CF modem, when there is one. */
CE_API(lineInitialize)
{
    if (!a0 || !a4)
        return 0x80000035u; /* LINEERR_INVALPOINTER */
    *PTR(CW, a0) = 0x7f000001u;
    *PTR(CW, a4) = modem() ? 1 : 0;
    return 0;
}
CE_API(lineNegotiateAPIVersion)
{
    if (a1 != 0 || !modem())
        return 0x80000002u; /* LINEERR_BADDEVICEID */
    if (a4)
        *PTR(CW, a4) = a3 >= 0x00020000u ? 0x00020000u : a3; /* the newest asked for, up to 2.0 */
    if (a5)
        memset(PTR(void, a5), 0, 8);
    return 0;
}
/* LINEDEVCAPS: sizes at 0/4/8, provider info 12/16, switch 20/24,
 * permanent ID 28, line name size/offset 32/36, string format 40. */
CE_API(lineGetDevCaps)
{
    uint8_t *caps = PTR(uint8_t, a4);
    if (a1 != 0 || !modem())
        return 0x80000002u;
    if (!caps || ce_u32(caps) < 48)
        return 0x8000004du; /* LINEERR_STRUCTURETOOSMALL */
    static const WCHAR name[] = u"Glaze CF Modem";
    CW total = ce_u32(caps), base = 236, need = base + sizeof(name);
    memset(caps + 4, 0, MIN(total, base) - 4);
    ce_put32(caps + 4, need);
    ce_put32(caps + 28, 1);
    ce_put32(caps + 40, 3); /* STRINGFORMAT_UNICODE */
    if (total >= need) {
        memcpy(caps + base, name, sizeof(name));
        ce_put32(caps + 32, sizeof(name));
        ce_put32(caps + 36, base);
        ce_put32(caps + 8, need);
    } else {
        ce_put32(caps + 8, MIN(total, base));
    }
    return 0;
}
/* lineTranslateAddress(app, device, version, address, card, flags,
 * LINETRANSLATEOUTPUT): canonical "+country (area) number" becomes the
 * digits to dial; the displayable string is the address as given. */
CE_API(lineTranslateAddress)
{
    const WCHAR *in = PTR(WCHAR, a3);
    uint8_t *out = PTR(uint8_t, a6);
    if (!in || !out || ce_u32(out) < 40)
        return 0x80000035u;
    WCHAR dial[128];
    unsigned n = 0, len = ce_wlen(in);
    for (unsigned i = 0; i < len && n + 1 < ARRAY_SIZE(dial); i++)
        if ((in[i] >= '0' && in[i] <= '9') || in[i] == '*' || in[i] == '#' || in[i] == ',' || in[i] == 'W' ||
            in[i] == 'P' || in[i] == 'T')
            dial[n++] = in[i];
    dial[n] = 0;
    CW total = ce_u32(out), d_bytes = (n + 1) * 2, s_bytes = (len + 1) * 2, need = 40 + d_bytes + s_bytes;
    memset(out + 4, 0, 36);
    ce_put32(out + 4, need);
    ce_put32(out + 8, 40);
    if (total >= need) {
        memcpy(out + 40, dial, d_bytes);
        memcpy(out + 40 + d_bytes, in, s_bytes);
        ce_put32(out + 8, need);
        ce_put32(out + 12, d_bytes);
        ce_put32(out + 16, 40);
        ce_put32(out + 20, s_bytes);
        ce_put32(out + 24, 40 + d_bytes);
    }
    return 0;
}

void *ce_resolve_ras(const char *dll, const char *name)
{
    static const struct ce_export exports[] = {
        CE_EXPORT(RasEnumEntries), CE_EXPORT(RasGetEntryProperties), CE_EXPORT(RasSetEntryProperties),
        CE_EXPORT(RasGetEntryDialParams), CE_EXPORT(RasSetEntryDialParams), CE_EXPORT(RasDial),
        CE_EXPORT(RasHangup), CE_EXPORT(RasGetConnectStatus), CE_EXPORT(RasEnumDevicesW),
        CE_EXPORT(lineInitialize), CE_EXPORT(lineNegotiateAPIVersion), CE_EXPORT(lineGetDevCaps),
        CE_EXPORT(lineTranslateAddress),
    };
    if (strcasecmp(dll, "coredll.dll"))
        return NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++)
        if (!strcmp(name, exports[i].name))
            return exports[i].address;
    return NULL;
}
