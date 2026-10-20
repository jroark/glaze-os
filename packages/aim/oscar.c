/*
 * The OSCAR engine (see oscar.h). Message layouts follow the AIM 5.x-era
 * protocol as the open-source clients and revival servers document it:
 * FLAP channels 1 (sign-on), 2 (SNAC), 4 (sign-off) and 5 (keepalive);
 * SNAC families 0x01 OSERVICE, 0x02 LOCATE, 0x03 BUDDY, 0x04 ICBM and 0x13
 * FEEDBAG, and 0x17 BUCP on the login server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "oscar.h"

#define FLAP_HDR        6
#define SNAC_HDR        10
#define PKT_MAX         4096
#define KEEPALIVE_MS    60000

enum { CH_SIGNON = 1, CH_SNAC = 2, CH_ERROR = 3, CH_SIGNOFF = 4, CH_KEEPALIVE = 5 };

enum {
    FAM_OSERVICE = 0x01, FAM_LOCATE = 0x02, FAM_BUDDY = 0x03, FAM_ICBM = 0x04,
    FAM_FEEDBAG = 0x13, FAM_BUCP = 0x17,
};

enum { CLASS_BUDDY = 0, CLASS_GROUP = 1 };
#define TLV_ORDER       0x00C8      /* feedbag: member ids of a group or the root */

#define CLIENT_ID       "AOL Instant Messenger, version 5.1.3036/WIN32"
#define AIM_MD5_STRING  "AOL Instant Messenger (SM)"
#define AWAY_MIME       "text/aolrtf; charset=\"us-ascii\""

/* ---- byte buffers ------------------------------------------------------- */

struct wbuf {
    uint8_t *p;
    size_t len, cap;
    bool overflow;
};

static void put(struct wbuf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        b->overflow = true;
        return;
    }
    if (n)
        memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void put8(struct wbuf *b, unsigned v)
{
    uint8_t x = (uint8_t)v;
    put(b, &x, 1);
}

static void put16(struct wbuf *b, unsigned v)
{
    uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 2);
}

static void put32(struct wbuf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 4);
}

static void put_tlv(struct wbuf *b, unsigned type, const void *data, size_t n)
{
    put16(b, type);
    put16(b, (unsigned)n);
    put(b, data, n);
}

static void put_tlv_str(struct wbuf *b, unsigned type, const char *s)
{
    put_tlv(b, type, s, strlen(s));
}

static void put_tlv16(struct wbuf *b, unsigned type, unsigned v)
{
    put16(b, type);
    put16(b, 2);
    put16(b, v);
}

static void put_tlv32(struct wbuf *b, unsigned type, uint32_t v)
{
    put16(b, type);
    put16(b, 4);
    put32(b, v);
}

static void put_sn(struct wbuf *b, const char *sn)
{
    size_t n = strlen(sn);
    put8(b, (unsigned)n);
    put(b, sn, n);
}

struct rbuf {
    const uint8_t *p;
    size_t len, off;
    bool bad;
};

static bool need(struct rbuf *r, size_t n)
{
    if (r->bad || r->len - r->off < n) {
        r->bad = true;
        return false;
    }
    return true;
}

static unsigned get8(struct rbuf *r)
{
    return need(r, 1) ? r->p[r->off++] : 0;
}

static unsigned get16(struct rbuf *r)
{
    if (!need(r, 2))
        return 0;
    unsigned v = (unsigned)r->p[r->off] << 8 | r->p[r->off + 1];
    r->off += 2;
    return v;
}

static uint32_t get32(struct rbuf *r)
{
    if (!need(r, 4))
        return 0;
    const uint8_t *p = r->p + r->off;
    r->off += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static const uint8_t *get_bytes(struct rbuf *r, size_t n)
{
    if (!need(r, n))
        return NULL;
    r->off += n;
    return r->p + r->off - n;
}

static unsigned be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

/* Value of TLV `type` in a TLV chain, or NULL. */
static const uint8_t *find_tlv(const uint8_t *p, size_t len, unsigned type, size_t *vlen)
{
    size_t off = 0;

    while (off + 4 <= len) {
        unsigned t = be16(p + off), n = be16(p + off + 2);
        if (off + 4 + n > len)
            break;
        if (t == type) {
            *vlen = n;
            return p + off + 4;
        }
        off += 4 + n;
    }
    return NULL;
}

static void copy_str(char *out, size_t n, const uint8_t *p, size_t len)
{
    if (!n)
        return;
    if (len >= n)
        len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
}

/* ---- names -------------------------------------------------------------- */

void oscar_norm(char *out, size_t n, const char *name)
{
    size_t o = 0;

    if (!n)
        return;
    for (; *name && o + 1 < n; name++)
        if (*name != ' ')
            out[o++] = (char)(*name >= 'A' && *name <= 'Z' ? *name - 'A' + 'a' : *name);
    out[o] = 0;
}

bool oscar_same_name(const char *a, const char *b)
{
    char na[OSCAR_SN_MAX * 2], nb[OSCAR_SN_MAX * 2];

    oscar_norm(na, sizeof(na), a);
    oscar_norm(nb, sizeof(nb), b);
    return !strcmp(na, nb);
}

struct oscar_buddy *oscar_find_buddy(struct oscar *o, const char *name)
{
    for (int i = 0; i < o->nbuddies; i++)
        if (oscar_same_name(o->buddies[i].name, name))
            return &o->buddies[i];
    return NULL;
}

struct oscar_group *oscar_find_group(struct oscar *o, const char *name)
{
    for (int i = 0; i < o->ngroups; i++)
        if (!strcasecmp(o->groups[i].name, name))
            return &o->groups[i];
    return NULL;
}

static struct oscar_group *group_by_id(struct oscar *o, unsigned gid)
{
    for (int i = 0; i < o->ngroups; i++)
        if (o->groups[i].gid == gid)
            return &o->groups[i];
    return NULL;
}

int oscar_idle_minutes(const struct oscar *o, const struct oscar_buddy *b)
{
    if (!b->online || !b->idle_min)
        return 0;
    return b->idle_min + (int)((o->now_ms - b->idle_since_ms) / 60000);
}

/* ---- sending ------------------------------------------------------------ */

static void emit(struct oscar *o, enum oscar_event_type type, const char *name, const char *text,
                 const char *text2, int flag)
{
    struct oscar_event ev = { type, name, text, text2, flag };
    o->ops.event(o->ops.ctx, &ev);
}

static void fail(struct oscar *o, const char *why)
{
    if (o->state == OSCAR_FAILED || o->state == OSCAR_IDLE)
        return;
    o->state = OSCAR_FAILED;
    emit(o, OSCAR_EV_FAILED, NULL, why, NULL, 0);
}

static void flap(struct oscar *o, int channel, const void *data, size_t len)
{
    uint8_t pkt[FLAP_HDR + PKT_MAX];

    if (o->state == OSCAR_FAILED || o->state == OSCAR_IDLE)
        return;
    if (len > PKT_MAX) {
        fail(o, "A request was too large.");
        return;
    }
    pkt[0] = '*';
    pkt[1] = (uint8_t)channel;
    pkt[2] = (uint8_t)(o->seq >> 8);
    pkt[3] = (uint8_t)o->seq;
    pkt[4] = (uint8_t)(len >> 8);
    pkt[5] = (uint8_t)len;
    memcpy(pkt + FLAP_HDR, data, len);
    o->seq = (uint16_t)((o->seq + 1) & 0x7FFF);
    o->last_send_ms = o->now_ms;
    if (o->ops.send(o->ops.ctx, pkt, FLAP_HDR + len) < 0)
        fail(o, "The connection was lost.");
}

/* Sends SNAC fam/sub with the body in b (which starts after the header). */
static uint32_t snac(struct oscar *o, unsigned fam, unsigned sub, const struct wbuf *body)
{
    uint8_t pkt[PKT_MAX];
    struct wbuf b = { pkt, 0, sizeof(pkt), false };
    uint32_t id = ++o->reqid;

    put16(&b, fam);
    put16(&b, sub);
    put16(&b, 0);
    put32(&b, id);
    if (body) {
        if (body->overflow) {
            fail(o, "A request was too large.");
            return id;
        }
        put(&b, body->p, body->len);
    }
    if (b.overflow)
        fail(o, "A request was too large.");
    else
        flap(o, CH_SNAC, pkt, b.len);
    return id;
}

#define BODY(name, size) \
    uint8_t name##_data[size]; \
    struct wbuf name = { name##_data, 0, sizeof(name##_data), false }

/* ---- sign-on ------------------------------------------------------------ */

void oscar_init(struct oscar *o, const struct oscar_ops *ops)
{
    memset(o, 0, sizeof(*o));
    o->ops = *ops;
}

static void reset_session(struct oscar *o)
{
    o->in_len = 0;
    o->skip = 0;
}

void oscar_login(struct oscar *o, const char *sn, const char *password)
{
    reset_session(o);
    strlcpy(o->sn, sn, sizeof(o->sn));
    strlcpy(o->password, password, sizeof(o->password));
    o->ngroups = o->nbuddies = 0;
    o->have_list = o->have_root = o->feedbag = false;
    o->max_iid = o->max_gid = 0;
    o->edits_pending = 0;
    o->seq = (uint16_t)((o->now_ms * 7919u) & 0x7FFF);
    o->state = OSCAR_LOGIN;
}

void oscar_bos_connected(struct oscar *o)
{
    if (o->state != OSCAR_REDIRECTED)
        return;
    reset_session(o);
    o->state = OSCAR_BOS;
    emit(o, OSCAR_EV_PROGRESS, NULL, "Starting services...", NULL, 0);
}

void oscar_closed(struct oscar *o)
{
    if (o->state == OSCAR_REDIRECTED || o->state == OSCAR_IDLE || o->state == OSCAR_FAILED)
        return;
    if (o->state == OSCAR_ONLINE) {
        o->state = OSCAR_IDLE;
        emit(o, OSCAR_EV_SIGNED_OFF, NULL, "The connection to the server was lost.", NULL, 0);
        return;
    }
    fail(o, "The server closed the connection.");
}

static void request_key(struct oscar *o)
{
    BODY(b, 128);

    put_tlv_str(&b, 0x0001, o->sn);
    put_tlv(&b, 0x004B, NULL, 0);
    put_tlv(&b, 0x005A, NULL, 0);
    snac(o, FAM_BUCP, 0x06, &b);
}

static void send_password(struct oscar *o, const uint8_t *key, size_t keylen)
{
    uint8_t buf[512], pass_hash[16], hash[16];
    size_t n = 0, aim = sizeof(AIM_MD5_STRING) - 1;
    BODY(b, 512);

    /* md5(key + md5(password) + "AOL Instant Messenger (SM)"), which the
     * empty TLV 0x4C announces. */
    if (keylen > sizeof(buf) - 16 - aim) {
        fail(o, "The server sent a bad sign-on key.");
        return;
    }
    oscar_md5(o->password, strlen(o->password), pass_hash);
    memcpy(buf, key, keylen);
    n = keylen;
    memcpy(buf + n, pass_hash, 16);
    n += 16;
    memcpy(buf + n, AIM_MD5_STRING, aim);
    n += aim;
    oscar_md5(buf, n, hash);
    memset(pass_hash, 0, sizeof(pass_hash));

    put_tlv_str(&b, 0x0001, o->sn);
    put_tlv(&b, 0x0025, hash, 16);
    put_tlv(&b, 0x004C, NULL, 0);
    put_tlv_str(&b, 0x0003, CLIENT_ID);
    put_tlv16(&b, 0x0016, 0x0109);
    put_tlv16(&b, 0x0017, 5);
    put_tlv16(&b, 0x0018, 1);
    put_tlv16(&b, 0x0019, 0);
    put_tlv16(&b, 0x001A, 3036);
    put_tlv32(&b, 0x0014, 0xD2);
    put_tlv_str(&b, 0x000F, "en");
    put_tlv_str(&b, 0x000E, "us");
    put16(&b, 0x004A);
    put16(&b, 1);
    put8(&b, 1);
    snac(o, FAM_BUCP, 0x02, &b);
}

static const char *login_error(unsigned code)
{
    switch (code) {
    case 0x01: case 0x04: case 0x05: return "The screen name or password is incorrect.";
    case 0x11: return "This screen name is suspended.";
    case 0x14: case 0x15: case 0x1A: return "The service is temporarily unavailable.";
    case 0x18: case 0x1D: return "You are signing on too often. Wait a few minutes and try again.";
    case 0x1C: return "The server requires a newer client.";
    default: return "The server refused the sign-on.";
    }
}

/* The login reply's TLVs, from SNAC 17,03 or a channel 4 FLAP. */
static void login_reply(struct oscar *o, const uint8_t *p, size_t len)
{
    size_t n;
    const uint8_t *v = find_tlv(p, len, 0x0008, &n);
    char host[OSCAR_HOST_MAX];

    if (v && n >= 2) {
        fail(o, login_error(be16(v)));
        return;
    }
    const uint8_t *addr = find_tlv(p, len, 0x0005, &n);
    size_t addr_len = n, cookie_len;
    const uint8_t *cookie = find_tlv(p, len, 0x0006, &cookie_len);
    if (!addr || !cookie || !cookie_len || cookie_len > sizeof(o->cookie)) {
        fail(o, "The server sent an incomplete sign-on reply.");
        return;
    }
    copy_str(host, sizeof(host), addr, addr_len);
    char *colon = strrchr(host, ':');
    o->bos_port = OSCAR_PORT;
    if (colon) {
        *colon = 0;
        o->bos_port = (uint16_t)atoi(colon + 1);
    }
    strlcpy(o->bos_host, host, sizeof(o->bos_host));
    memcpy(o->cookie, cookie, cookie_len);
    o->cookie_len = (uint16_t)cookie_len;
    /* The password isn't needed past this point. */
    memset(o->password, 0, sizeof(o->password));
    o->state = OSCAR_REDIRECTED;
    emit(o, OSCAR_EV_REDIRECT, NULL, NULL, NULL, 0);
}

static void bucp(struct oscar *o, unsigned sub, struct rbuf *r)
{
    if (sub == 0x07) {
        unsigned n = get16(r);
        const uint8_t *key = get_bytes(r, n);
        if (!key) {
            fail(o, "The server sent a bad sign-on key.");
            return;
        }
        send_password(o, key, n);
    } else if (sub == 0x03) {
        login_reply(o, r->p + r->off, r->len - r->off);
    } else if (sub == 0x01) {
        fail(o, "The server refused the sign-on.");
    }
}

/* ---- BOS service setup -------------------------------------------------- */

static const struct { uint16_t fam, version; } families[] = {
    { FAM_OSERVICE, 4 }, { FAM_FEEDBAG, 4 }, { FAM_LOCATE, 1 }, { FAM_BUDDY, 1 }, { FAM_ICBM, 1 },
};
#define NFAMILIES ((int)(sizeof(families) / sizeof(families[0])))

static void send_profile(struct oscar *o)
{
    BODY(b, PKT_MAX - SNAC_HDR);
    char html[OSCAR_INFO_MAX * 2];

    put_tlv_str(&b, 0x0001, AWAY_MIME);
    size_t n = oscar_text_to_html(html, sizeof(html), o->profile);
    put_tlv(&b, 0x0002, html, n);
    snac(o, FAM_LOCATE, 0x04, &b);
}

static void client_ready(struct oscar *o)
{
    BODY(b, 128);

    for (int i = 0; i < NFAMILIES; i++) {
        if (families[i].fam == FAM_FEEDBAG && !o->feedbag)
            continue;
        put16(&b, families[i].fam);
        put16(&b, families[i].version);
        put16(&b, 0x0110);
        put16(&b, 0x164F);
    }
    snac(o, FAM_OSERVICE, 0x02, &b);
    o->state = OSCAR_ONLINE;
    emit(o, OSCAR_EV_ONLINE, NULL, NULL, NULL, 0);
    emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
}

/* Once the buddy list is in: turn on presence and messaging. */
static void finish_setup(struct oscar *o)
{
    BODY(b, 32);

    if (o->feedbag)
        snac(o, FAM_FEEDBAG, 0x07, NULL);
    /* ICBM parameters for channel 0 (all channels): channel messages,
     * missed calls and typing notifications. */
    put16(&b, 0);
    put32(&b, 0x0000000B);
    put16(&b, 8000);
    put16(&b, 999);
    put16(&b, 999);
    put32(&b, 0);
    snac(o, FAM_ICBM, 0x02, &b);
    send_profile(o);
    client_ready(o);
}

static void host_online(struct oscar *o, struct rbuf *r)
{
    BODY(b, 128);

    o->feedbag = false;
    while (r->off + 2 <= r->len) {
        unsigned fam = get16(r);
        if (fam == FAM_FEEDBAG)
            o->feedbag = true;
        for (int i = 0; i < NFAMILIES; i++)
            if (families[i].fam == fam) {
                put16(&b, fam);
                put16(&b, families[i].version);
            }
    }
    snac(o, FAM_OSERVICE, 0x17, &b);
}

static void rate_info(struct oscar *o, struct rbuf *r)
{
    BODY(b, 64);
    unsigned n = get16(r);

    o->nrates = 0;
    for (unsigned i = 0; i < n && !r->bad; i++) {
        unsigned id = get16(r);
        get_bytes(r, 33);       /* window .. state */
        if (!r->bad && o->nrates < (int)(sizeof(o->rate_ids) / sizeof(o->rate_ids[0])))
            o->rate_ids[o->nrates++] = (uint16_t)id;
    }
    for (int i = 0; i < o->nrates; i++)
        put16(&b, o->rate_ids[i]);
    snac(o, FAM_OSERVICE, 0x08, &b);

    snac(o, FAM_OSERVICE, 0x0E, NULL);  /* self info */
    snac(o, FAM_LOCATE, 0x02, NULL);
    snac(o, FAM_BUDDY, 0x02, NULL);
    snac(o, FAM_ICBM, 0x04, NULL);
    if (o->feedbag) {
        snac(o, FAM_FEEDBAG, 0x02, NULL);
        snac(o, FAM_FEEDBAG, 0x04, NULL);
    } else {
        o->have_list = true;
        finish_setup(o);
    }
}

static void oservice(struct oscar *o, unsigned sub, struct rbuf *r)
{
    switch (sub) {
    case 0x03:
        host_online(o, r);
        break;
    case 0x18:
        snac(o, FAM_OSERVICE, 0x06, NULL);
        break;
    case 0x07:
        if (o->state == OSCAR_BOS)
            rate_info(o, r);
        break;
    }
}

/* ---- feedbag (the server-side buddy list) -------------------------------- */

struct item {
    char name[OSCAR_SN_MAX];
    unsigned gid, iid, cls;
    const uint8_t *tlv;
    size_t tlv_len;
};

static bool get_item(struct rbuf *r, struct item *it)
{
    unsigned n = get16(r);
    const uint8_t *name = get_bytes(r, n);

    it->gid = get16(r);
    it->iid = get16(r);
    it->cls = get16(r);
    unsigned tn = get16(r);
    it->tlv = get_bytes(r, tn);
    it->tlv_len = tn;
    if (r->bad)
        return false;
    copy_str(it->name, sizeof(it->name), name, n);
    return true;
}

static void put_item(struct wbuf *b, const char *name, unsigned gid, unsigned iid, unsigned cls,
                     const uint16_t *order, int norder)
{
    size_t n = strlen(name);

    put16(b, (unsigned)n);
    put(b, name, n);
    put16(b, gid);
    put16(b, iid);
    put16(b, cls);
    if (norder < 0) {
        put16(b, 0);
        return;
    }
    put16(b, 4 + 2 * (unsigned)norder);
    put16(b, TLV_ORDER);
    put16(b, 2 * (unsigned)norder);
    for (int i = 0; i < norder; i++)
        put16(b, order[i]);
}

static void track_ids(struct oscar *o, const struct item *it)
{
    if (it->iid > o->max_iid)
        o->max_iid = (uint16_t)it->iid;
    if (it->gid > o->max_gid)
        o->max_gid = (uint16_t)it->gid;
}

/* Sort keys from the order TLVs, kept while a list is being read. */
static uint16_t group_rank[OSCAR_GROUPS];
static uint16_t buddy_rank[OSCAR_BUDDIES];

static unsigned rank_in(const uint8_t *order, size_t n, unsigned id)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        if (be16(order + i) == id)
            return (unsigned)(i / 2);
    return 0xFFFF;
}

static void add_item(struct oscar *o, const struct item *it)
{
    track_ids(o, it);
    if (it->cls == CLASS_GROUP && it->gid == 0) {
        o->have_root = true;
        return;
    }
    if (it->cls == CLASS_GROUP && !group_by_id(o, it->gid) && o->ngroups < OSCAR_GROUPS) {
        struct oscar_group *g = &o->groups[o->ngroups++];
        strlcpy(g->name, it->name, sizeof(g->name));
        g->gid = (uint16_t)it->gid;
        return;
    }
    if (it->cls == CLASS_BUDDY && it->name[0] && o->nbuddies < OSCAR_BUDDIES) {
        for (int i = 0; i < o->nbuddies; i++)
            if (o->buddies[i].gid == it->gid && oscar_same_name(o->buddies[i].name, it->name))
                return;
        struct oscar_buddy *b = &o->buddies[o->nbuddies++];
        memset(b, 0, sizeof(*b));
        strlcpy(b->name, it->name, sizeof(b->name));
        b->gid = (uint16_t)it->gid;
        b->iid = (uint16_t)it->iid;
        /* Someone already listed in another group shares their presence. */
        for (int i = 0; i < o->nbuddies - 1; i++)
            if (oscar_same_name(o->buddies[i].name, b->name)) {
                b->online = o->buddies[i].online;
                b->away = o->buddies[i].away;
                b->idle_min = o->buddies[i].idle_min;
                b->idle_since_ms = o->buddies[i].idle_since_ms;
            }
    }
}

static void remove_item(struct oscar *o, const struct item *it)
{
    if (it->cls == CLASS_GROUP) {
        for (int i = 0; i < o->ngroups; i++)
            if (o->groups[i].gid == it->gid) {
                memmove(&o->groups[i], &o->groups[i + 1], (size_t)(o->ngroups - i - 1) * sizeof(o->groups[0]));
                o->ngroups--;
                break;
            }
        return;
    }
    if (it->cls != CLASS_BUDDY)
        return;
    for (int i = 0; i < o->nbuddies; i++)
        if (o->buddies[i].gid == it->gid && o->buddies[i].iid == it->iid) {
            memmove(&o->buddies[i], &o->buddies[i + 1], (size_t)(o->nbuddies - i - 1) * sizeof(o->buddies[0]));
            o->nbuddies--;
            return;
        }
}

/* Puts groups and buddies in the order the root and group items give. */
static void sort_list(struct oscar *o, const uint8_t *root, size_t root_len)
{
    for (int i = 0; i < o->ngroups; i++)
        group_rank[i] = (uint16_t)rank_in(root, root_len, o->groups[i].gid);
    /* Insertion sorts: the lists are small and mostly in order already. */
    for (int i = 1; i < o->ngroups; i++)
        for (int j = i; j > 0 && group_rank[j - 1] > group_rank[j]; j--) {
            struct oscar_group g = o->groups[j];
            uint16_t k = group_rank[j];
            o->groups[j] = o->groups[j - 1];
            group_rank[j] = group_rank[j - 1];
            o->groups[j - 1] = g;
            group_rank[j - 1] = k;
        }
    /* Buddies: by group position, then by buddy_rank (set by the caller). */
    for (int i = 1; i < o->nbuddies; i++)
        for (int j = i; j > 0; j--) {
            struct oscar_buddy *a = &o->buddies[j - 1], *b = &o->buddies[j];
            int ga = (int)(group_by_id(o, a->gid) - o->groups), gb = (int)(group_by_id(o, b->gid) - o->groups);
            if (ga < gb || (ga == gb && buddy_rank[j - 1] <= buddy_rank[j]))
                break;
            struct oscar_buddy t = *a;
            uint16_t k = buddy_rank[j - 1];
            *a = *b;
            buddy_rank[j - 1] = buddy_rank[j];
            *b = t;
            buddy_rank[j] = k;
        }
}

/* Raw feedbag, accumulated over the replies until the last one. */
static uint8_t list_raw[32768];
static size_t list_raw_len;
static bool list_partial;       /* more replies are due */

static void parse_list(struct oscar *o)
{
    struct rbuf r = { list_raw, list_raw_len, 0, false };
    struct item it;
    uint8_t root[2 * OSCAR_GROUPS * 2];
    size_t root_len = 0;

    o->ngroups = o->nbuddies = 0;
    o->have_root = false;
    while (r.off < r.len && get_item(&r, &it)) {
        if (it.cls == CLASS_GROUP && it.gid == 0) {
            size_t n;
            const uint8_t *v = find_tlv(it.tlv, it.tlv_len, TLV_ORDER, &n);
            if (v) {
                root_len = n < sizeof(root) ? n : sizeof(root);
                memcpy(root, v, root_len);
            }
        }
        add_item(o, &it);
    }
    /* Second pass for each buddy's position in its group's order. */
    r.off = 0;
    r.bad = false;
    while (r.off < r.len && get_item(&r, &it)) {
        if (it.cls != CLASS_GROUP || it.gid == 0)
            continue;
        size_t n;
        const uint8_t *v = find_tlv(it.tlv, it.tlv_len, TLV_ORDER, &n);
        for (int i = 0; i < o->nbuddies; i++)
            if (o->buddies[i].gid == it.gid)
                buddy_rank[i] = (uint16_t)(v ? rank_in(v, n, o->buddies[i].iid) : 0xFFFF);
    }
    /* Buddies outside any group aren't shown (or editable) by AIM either. */
    int keep = 0;
    for (int i = 0; i < o->nbuddies; i++)
        if (group_by_id(o, o->buddies[i].gid)) {
            buddy_rank[keep] = buddy_rank[i];
            o->buddies[keep++] = o->buddies[i];
        }
    o->nbuddies = keep;
    sort_list(o, root, root_len);
}

static void list_reply(struct oscar *o, unsigned flags, struct rbuf *r)
{
    /* Each reply: version byte, item count, items, and (in the last) a
     * modification time. Keep only the items. */
    get8(r);
    unsigned count = get16(r);
    size_t start = r->off;
    struct item it;

    if (!list_partial)
        list_raw_len = 0;
    list_partial = flags & 0x0001;
    for (unsigned i = 0; i < count && get_item(r, &it); i++)
        ;
    if (r->bad) {
        fail(o, "The server sent a bad buddy list.");
        return;
    }
    size_t n = r->off - start;
    if (list_raw_len + n <= sizeof(list_raw)) {
        memcpy(list_raw + list_raw_len, r->p + start, n);
        list_raw_len += n;
    }
    if (list_partial)
        return;
    parse_list(o);
    if (!o->have_list) {
        o->have_list = true;
        finish_setup(o);
    } else {
        emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
    }
}

/* Changes another session of this account made, or the server's echo. */
static void list_change(struct oscar *o, unsigned sub, struct rbuf *r)
{
    struct item it;
    bool changed = false;

    while (r->off < r->len && get_item(r, &it)) {
        if (sub == 0x08) {
            if (it.cls == CLASS_BUDDY && !group_by_id(o, it.gid)) {
                track_ids(o, &it);
                continue;
            }
            if (it.cls == CLASS_BUDDY && oscar_find_buddy(o, it.name)) {
                bool dup = false;
                for (int i = 0; i < o->nbuddies; i++)
                    if (o->buddies[i].gid == it.gid && o->buddies[i].iid == it.iid)
                        dup = true;
                if (dup)
                    continue;
            }
            add_item(o, &it);
        } else if (sub == 0x0A) {
            remove_item(o, &it);
        } else {
            track_ids(o, &it);
            continue;
        }
        changed = true;
    }
    if (changed)
        emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
}

static void edit_ack(struct oscar *o, struct rbuf *r)
{
    bool ok = true;

    while (r->off + 2 <= r->len)
        if (get16(r) != 0)
            ok = false;
    if (o->edits_pending > 0)
        o->edits_pending--;
    if (!ok) {
        /* Our optimistic copy is wrong now; fetch the server's. */
        o->edits_pending = 0;
        list_partial = false;
        snac(o, FAM_FEEDBAG, 0x04, NULL);
        emit(o, OSCAR_EV_EDITED, NULL, NULL, NULL, -1);
        return;
    }
    if (!o->edits_pending)
        emit(o, OSCAR_EV_EDITED, NULL, NULL, NULL, 0);
}

static void feedbag(struct oscar *o, unsigned sub, unsigned flags, struct rbuf *r)
{
    switch (sub) {
    case 0x06:
        list_reply(o, flags, r);
        break;
    case 0x0E:
        edit_ack(o, r);
        break;
    case 0x08: case 0x09: case 0x0A:
        if (o->state == OSCAR_ONLINE)
            list_change(o, sub, r);
        break;
    case 0x01:
        /* No list at all (a new account): carry on without one. */
        if (!o->have_list) {
            o->have_list = true;
            finish_setup(o);
        }
        break;
    }
}

/* Member ids of group gid in list order (buddies), or of the root. */
static int group_order(struct oscar *o, unsigned gid, uint16_t *out, int max)
{
    int n = 0;

    for (int i = 0; i < o->nbuddies && n < max; i++)
        if (o->buddies[i].gid == gid)
            out[n++] = o->buddies[i].iid;
    return n;
}

static int root_order(struct oscar *o, uint16_t *out)
{
    for (int i = 0; i < o->ngroups; i++)
        out[i] = o->groups[i].gid;
    return o->ngroups;
}

/* Sends one edit SNAC (13,08 add / 09 update / 0A delete). */
static void edit(struct oscar *o, unsigned sub, const struct wbuf *b)
{
    o->edits_pending++;
    snac(o, FAM_FEEDBAG, sub, b);
}

static void update_group(struct oscar *o, const struct oscar_group *g)
{
    uint16_t order[OSCAR_BUDDIES];
    BODY(b, PKT_MAX - SNAC_HDR);

    put_item(&b, g->name, g->gid, 0, CLASS_GROUP, order, group_order(o, g->gid, order, OSCAR_BUDDIES));
    edit(o, 0x09, &b);
}

static void update_root(struct oscar *o)
{
    uint16_t order[OSCAR_GROUPS];
    BODY(b, 256);

    put_item(&b, "", 0, 0, CLASS_GROUP, order, root_order(o, order));
    edit(o, o->have_root ? 0x09 : 0x08, &b);
    o->have_root = true;
}

static bool can_edit(struct oscar *o)
{
    return o->state == OSCAR_ONLINE && o->feedbag && o->have_list;
}

static struct oscar_group *create_group(struct oscar *o, const char *name)
{
    if (o->ngroups >= OSCAR_GROUPS || !name[0] || strlen(name) >= OSCAR_GROUP_NAME)
        return NULL;
    struct oscar_group *g = &o->groups[o->ngroups++];
    BODY(b, 128);

    strlcpy(g->name, name, sizeof(g->name));
    g->gid = ++o->max_gid;
    put_item(&b, g->name, g->gid, 0, CLASS_GROUP, NULL, 0);
    edit(o, 0x08, &b);
    update_root(o);
    return g;
}

int oscar_add_group(struct oscar *o, const char *group)
{
    if (!can_edit(o) || oscar_find_group(o, group))
        return -1;
    snac(o, FAM_FEEDBAG, 0x11, NULL);
    struct oscar_group *g = create_group(o, group);
    snac(o, FAM_FEEDBAG, 0x12, NULL);
    if (!g)
        return -1;
    emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
    return 0;
}

int oscar_add_buddy(struct oscar *o, const char *name, const char *group)
{
    char norm[OSCAR_SN_MAX];

    oscar_norm(norm, sizeof(norm), name);
    if (!can_edit(o) || !norm[0] || strlen(name) >= OSCAR_SN_MAX || o->nbuddies >= OSCAR_BUDDIES)
        return -1;
    struct oscar_group *g = oscar_find_group(o, group);
    for (int i = 0; g && i < o->nbuddies; i++)
        if (o->buddies[i].gid == g->gid && oscar_same_name(o->buddies[i].name, name))
            return -1;

    snac(o, FAM_FEEDBAG, 0x11, NULL);
    if (!g && !(g = create_group(o, group))) {
        snac(o, FAM_FEEDBAG, 0x12, NULL);
        return -1;
    }
    struct oscar_buddy *b;
    /* Insert after the group's last buddy so the list stays grouped. */
    int at = o->nbuddies;
    for (int i = 0; i < o->nbuddies; i++)
        if (o->buddies[i].gid == g->gid)
            at = i + 1;
    if (at == o->nbuddies) {
        int gi = (int)(g - o->groups);
        for (int i = 0; i < o->nbuddies; i++)
            if ((int)(group_by_id(o, o->buddies[i].gid) - o->groups) > gi) {
                at = i;
                break;
            }
    }
    memmove(&o->buddies[at + 1], &o->buddies[at], (size_t)(o->nbuddies - at) * sizeof(o->buddies[0]));
    o->nbuddies++;
    b = &o->buddies[at];
    memset(b, 0, sizeof(*b));
    strlcpy(b->name, name, sizeof(b->name));
    b->gid = g->gid;
    b->iid = ++o->max_iid;
    struct oscar_buddy *other = NULL;
    for (int i = 0; i < o->nbuddies; i++)
        if (i != at && oscar_same_name(o->buddies[i].name, name))
            other = &o->buddies[i];
    if (other) {
        b->online = other->online;
        b->away = other->away;
        b->idle_min = other->idle_min;
        b->idle_since_ms = other->idle_since_ms;
    }

    BODY(w, 128);
    put_item(&w, b->name, b->gid, b->iid, CLASS_BUDDY, NULL, -1);
    edit(o, 0x08, &w);
    update_group(o, g);
    snac(o, FAM_FEEDBAG, 0x12, NULL);
    emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
    return 0;
}

static void delete_buddy_at(struct oscar *o, int i)
{
    struct oscar_buddy b = o->buddies[i];
    BODY(w, 128);

    memmove(&o->buddies[i], &o->buddies[i + 1], (size_t)(o->nbuddies - i - 1) * sizeof(b));
    o->nbuddies--;
    put_item(&w, b.name, b.gid, b.iid, CLASS_BUDDY, NULL, -1);
    edit(o, 0x0A, &w);
}

int oscar_remove_buddy(struct oscar *o, const char *name)
{
    struct oscar_buddy *b = oscar_find_buddy(o, name);

    if (!can_edit(o) || !b)
        return -1;
    unsigned gid = b->gid;
    snac(o, FAM_FEEDBAG, 0x11, NULL);
    delete_buddy_at(o, (int)(b - o->buddies));
    struct oscar_group *g = group_by_id(o, gid);
    if (g)
        update_group(o, g);
    snac(o, FAM_FEEDBAG, 0x12, NULL);
    emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
    return 0;
}

int oscar_remove_group(struct oscar *o, const char *group)
{
    struct oscar_group *g = oscar_find_group(o, group);

    if (!can_edit(o) || !g)
        return -1;
    struct oscar_group copy = *g;
    snac(o, FAM_FEEDBAG, 0x11, NULL);
    for (int i = o->nbuddies - 1; i >= 0; i--)
        if (o->buddies[i].gid == copy.gid)
            delete_buddy_at(o, i);
    int gi = (int)(g - o->groups);
    memmove(&o->groups[gi], &o->groups[gi + 1], (size_t)(o->ngroups - gi - 1) * sizeof(*g));
    o->ngroups--;
    BODY(w, 128);
    put_item(&w, copy.name, copy.gid, 0, CLASS_GROUP, NULL, -1);
    edit(o, 0x0A, &w);
    update_root(o);
    snac(o, FAM_FEEDBAG, 0x12, NULL);
    emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
    return 0;
}

/* ---- presence ----------------------------------------------------------- */

struct user_info {
    char name[OSCAR_SN_MAX];
    bool away;
    unsigned idle;
};

/* A user info block: name, warning level, then a counted TLV chain. */
static bool get_user_info(struct rbuf *r, struct user_info *u)
{
    unsigned n = get8(r);
    const uint8_t *name = get_bytes(r, n);

    get16(r);
    unsigned count = get16(r);
    size_t start = r->off;
    for (unsigned i = 0; i < count && !r->bad; i++) {
        get16(r);
        get_bytes(r, get16(r));
    }
    if (r->bad)
        return false;
    copy_str(u->name, sizeof(u->name), name, n);
    const uint8_t *tlv = r->p + start;
    size_t len = r->off - start, vn;
    const uint8_t *v = find_tlv(tlv, len, 0x0001, &vn);
    u->away = v && vn >= 2 && (be16(v + vn - 2) & 0x0020);
    v = find_tlv(tlv, len, 0x0006, &vn);
    if (v && vn == 4 && (be16(v + 2) & 0x0001))
        u->away = true;
    v = find_tlv(tlv, len, 0x0004, &vn);
    u->idle = v && vn >= 2 ? be16(v) : 0;
    return true;
}

static void buddy_service(struct oscar *o, unsigned sub, struct rbuf *r)
{
    struct user_info u;
    bool changed = false;

    if (sub != 0x0B && sub != 0x0C)
        return;
    while (r->off < r->len && get_user_info(r, &u)) {
        for (int i = 0; i < o->nbuddies; i++) {
            struct oscar_buddy *b = &o->buddies[i];
            if (!oscar_same_name(b->name, u.name))
                continue;
            b->online = sub == 0x0B;
            b->away = b->online && u.away;
            b->idle_min = (uint16_t)(b->online ? u.idle : 0);
            b->idle_since_ms = o->now_ms;
            changed = true;
        }
    }
    if (changed)
        emit(o, OSCAR_EV_BUDDIES, NULL, NULL, NULL, 0);
}

/* ---- messages ----------------------------------------------------------- */

static void incoming_im(struct oscar *o, struct rbuf *r)
{
    struct user_info u;
    static char text[OSCAR_MSG_MAX];

    get_bytes(r, 8);
    unsigned channel = get16(r);
    if (!get_user_info(r, &u) || channel != 1)
        return;
    const uint8_t *tlv = r->p + r->off;
    size_t len = r->len - r->off, n;
    const uint8_t *data = find_tlv(tlv, len, 0x0002, &n);
    bool autoresp = find_tlv(tlv, len, 0x0004, &(size_t){ 0 }) != NULL;
    if (!data)
        return;

    /* Fragments: id, version, length; id 1 holds charset, subset, text. */
    struct rbuf f = { data, n, 0, false };
    size_t used = 0;
    text[0] = 0;
    while (f.off + 4 <= f.len) {
        unsigned id = get8(&f);
        get8(&f);
        unsigned flen = get16(&f);
        const uint8_t *frag = get_bytes(&f, flen);
        if (!frag || id != 1 || flen < 4)
            continue;
        oscar_html_to_text(text + used, sizeof(text) - used, frag + 4, flen - 4, (uint16_t)be16(frag));
        used = strlen(text);
    }
    emit(o, OSCAR_EV_IM, u.name, text, NULL, autoresp);
}

static void typing(struct oscar *o, struct rbuf *r)
{
    get_bytes(r, 8);
    get16(r);
    unsigned n = get8(r);
    const uint8_t *name = get_bytes(r, n);
    unsigned type = get16(r);
    char sn[OSCAR_SN_MAX];

    if (r->bad)
        return;
    copy_str(sn, sizeof(sn), name, n);
    emit(o, OSCAR_EV_TYPING, sn, NULL, NULL, (int)type);
}

static void icbm_error(struct oscar *o, uint32_t reqid, struct rbuf *r)
{
    unsigned code = get16(r);
    const char *why = code == 0x0004 ? "is not signed on." : code == 0x0003 || code == 0x0002
                    ? "could not be reached (rate limit). Try again shortly." : "could not be sent the message.";

    for (int i = 0; i < (int)(sizeof(o->sent) / sizeof(o->sent[0])); i++)
        if (o->sent[i].reqid == reqid && o->sent[i].name[0]) {
            emit(o, OSCAR_EV_NOT_SENT, o->sent[i].name, why, NULL, (int)code);
            return;
        }
}

static void icbm(struct oscar *o, unsigned sub, uint32_t reqid, struct rbuf *r)
{
    switch (sub) {
    case 0x07: incoming_im(o, r); break;
    case 0x14: typing(o, r); break;
    case 0x01: icbm_error(o, reqid, r); break;
    }
}

int oscar_send_im(struct oscar *o, const char *to, const char *text, bool autoresp)
{
    static char html[OSCAR_MSG_MAX * 3];
    BODY(b, PKT_MAX - SNAC_HDR);
    uint8_t cookie[8];
    bool latin = false;

    if (o->state != OSCAR_ONLINE || !to[0] || strlen(to) >= OSCAR_SN_MAX)
        return -1;
    size_t n = oscar_text_to_html(html, sizeof(html), text);
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)html[i] >= 0x80)
            latin = true;
    for (int i = 0; i < 8; i++)
        cookie[i] = (uint8_t)('0' + (o->now_ms + o->reqid * 31u + (uint32_t)i * 7u) % 10);

    put(&b, cookie, 8);
    put16(&b, 1);
    put_sn(&b, to);
    /* TLV 2: a capabilities fragment, then the text fragment. */
    put16(&b, 0x0002);
    put16(&b, (unsigned)(5 + 4 + 4 + n));
    put8(&b, 0x05);
    put8(&b, 0x01);
    put16(&b, 1);
    put8(&b, 0x01);
    put8(&b, 0x01);
    put8(&b, 0x01);
    put16(&b, (unsigned)(4 + n));
    put16(&b, latin ? 3 : 0);
    put16(&b, 0);
    put(&b, html, n);
    if (autoresp)
        put_tlv(&b, 0x0004, NULL, 0);
    if (b.overflow)
        return -1;
    uint32_t id = snac(o, FAM_ICBM, 0x06, &b);
    int slot = o->sent_next++ % (int)(sizeof(o->sent) / sizeof(o->sent[0]));
    o->sent[slot].reqid = id;
    strlcpy(o->sent[slot].name, to, sizeof(o->sent[slot].name));
    return 0;
}

int oscar_send_typing(struct oscar *o, const char *to, int state)
{
    BODY(b, 64);

    if (o->state != OSCAR_ONLINE || strlen(to) >= OSCAR_SN_MAX)
        return -1;
    for (int i = 0; i < 8; i++)
        put8(&b, 0);
    put16(&b, 1);
    put_sn(&b, to);
    put16(&b, (unsigned)state);
    snac(o, FAM_ICBM, 0x14, &b);
    return 0;
}

/* ---- locate ------------------------------------------------------------- */

int oscar_set_profile(struct oscar *o, const char *text)
{
    strlcpy(o->profile, text ? text : "", sizeof(o->profile));
    if (o->state == OSCAR_ONLINE)
        send_profile(o);
    return 0;
}

int oscar_set_away(struct oscar *o, const char *text)
{
    static char html[OSCAR_INFO_MAX * 2];
    BODY(b, PKT_MAX - SNAC_HDR);

    if (o->state != OSCAR_ONLINE)
        return -1;
    put_tlv_str(&b, 0x0003, AWAY_MIME);
    if (text && text[0])
        put_tlv(&b, 0x0004, html, oscar_text_to_html(html, sizeof(html), text));
    else
        put_tlv(&b, 0x0004, NULL, 0);
    snac(o, FAM_LOCATE, 0x04, &b);
    return 0;
}

int oscar_get_info(struct oscar *o, const char *name)
{
    BODY(b, 64);

    if (o->state != OSCAR_ONLINE || strlen(name) >= OSCAR_SN_MAX)
        return -1;
    put32(&b, 0x00000003);      /* profile and away message */
    put_sn(&b, name);
    snac(o, FAM_LOCATE, 0x15, &b);
    return 0;
}

/* Profile or away text with the charset its MIME type names. */
static void info_text(char *out, size_t n, const uint8_t *tlv, size_t len, unsigned mime_type, unsigned text_type)
{
    size_t mn, tn;
    const uint8_t *mime = find_tlv(tlv, len, mime_type, &mn);
    const uint8_t *text = find_tlv(tlv, len, text_type, &tn);
    uint16_t charset = 0;
    char m[64];

    out[0] = 0;
    if (!text)
        return;
    if (mime) {
        copy_str(m, sizeof(m), mime, mn);
        if (strstr(m, "unicode"))
            charset = 2;
    }
    oscar_html_to_text(out, n, text, tn, charset);
}

static void user_info_reply(struct oscar *o, struct rbuf *r)
{
    struct user_info u;
    static char profile[OSCAR_INFO_MAX], away[OSCAR_INFO_MAX];

    if (!get_user_info(r, &u))
        return;
    const uint8_t *tlv = r->p + r->off;
    size_t len = r->len - r->off;
    info_text(profile, sizeof(profile), tlv, len, 0x0001, 0x0002);
    info_text(away, sizeof(away), tlv, len, 0x0003, 0x0004);
    emit(o, OSCAR_EV_INFO, u.name, profile, away, 0);
}

static void locate(struct oscar *o, unsigned sub, struct rbuf *r)
{
    if (sub == 0x06)
        user_info_reply(o, r);
    else if (sub == 0x01)
        emit(o, OSCAR_EV_INFO, "", "", "", -1);     /* not signed on, or no such user */
}

/* ---- input -------------------------------------------------------------- */

static void signon_frame(struct oscar *o)
{
    BODY(b, OSCAR_COOKIE_MAX + 16);

    put32(&b, 1);
    if (o->state == OSCAR_LOGIN) {
        flap(o, CH_SIGNON, b.p, b.len);
        emit(o, OSCAR_EV_PROGRESS, NULL, "Verifying password...", NULL, 0);
        request_key(o);
    } else if (o->state == OSCAR_BOS) {
        put_tlv(&b, 0x0006, o->cookie, o->cookie_len);
        flap(o, CH_SIGNON, b.p, b.len);
    }
}

static void signoff_frame(struct oscar *o, const uint8_t *p, size_t len)
{
    if (o->state == OSCAR_LOGIN) {
        login_reply(o, p, len);
        return;
    }
    size_t n;
    const uint8_t *v = find_tlv(p, len, 0x0009, &n);
    const char *why = v && n >= 2 && be16(v) == 1 ? "You signed on from another location."
                                                  : "The server ended the session.";
    if (o->state == OSCAR_ONLINE) {
        o->state = OSCAR_IDLE;
        emit(o, OSCAR_EV_SIGNED_OFF, NULL, why, NULL, 0);
    } else {
        fail(o, why);
    }
}

static void snac_frame(struct oscar *o, const uint8_t *p, size_t len)
{
    struct rbuf r = { p, len, 0, false };
    unsigned fam = get16(&r), sub = get16(&r), flags = get16(&r);
    uint32_t reqid = get32(&r);

    if (r.bad)
        return;
    /* Flag 0x8000: a length-prefixed block of extra TLVs comes first. */
    if (flags & 0x8000)
        get_bytes(&r, get16(&r));
    if (r.bad)
        return;
    switch (fam) {
    case FAM_BUCP: if (o->state == OSCAR_LOGIN) bucp(o, sub, &r); break;
    case FAM_OSERVICE: oservice(o, sub, &r); break;
    case FAM_FEEDBAG: feedbag(o, sub, flags, &r); break;
    case FAM_BUDDY: buddy_service(o, sub, &r); break;
    case FAM_ICBM: icbm(o, sub, reqid, &r); break;
    case FAM_LOCATE: locate(o, sub, &r); break;
    }
}

void oscar_input(struct oscar *o, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len && o->state != OSCAR_FAILED && o->state != OSCAR_IDLE && o->state != OSCAR_REDIRECTED) {
        if (o->skip) {
            size_t k = o->skip < len ? o->skip : len;
            o->skip -= k;
            p += k;
            len -= k;
            continue;
        }
        size_t want = FLAP_HDR;
        if (o->in_len >= FLAP_HDR)
            want = FLAP_HDR + be16(o->in + 4);
        size_t take = want - o->in_len < len ? want - o->in_len : len;
        memcpy(o->in + o->in_len, p, take);
        o->in_len += take;
        p += take;
        len -= take;
        if (o->in_len < want)
            continue;
        if (want == FLAP_HDR) {
            if (o->in[0] != '*') {
                fail(o, "The server sent something that isn't AIM.");
                return;
            }
            size_t body = be16(o->in + 4);
            if (FLAP_HDR + body > sizeof(o->in)) {
                o->skip = body;
                o->in_len = 0;
            }
            if (body)
                continue;
        }
        unsigned channel = o->in[1];
        size_t body = o->in_len - FLAP_HDR;
        o->in_len = 0;
        switch (channel) {
        case CH_SIGNON: signon_frame(o); break;
        case CH_SNAC: snac_frame(o, o->in + FLAP_HDR, body); break;
        case CH_SIGNOFF: signoff_frame(o, o->in + FLAP_HDR, body); break;
        case CH_ERROR: fail(o, "The server reported an error."); break;
        }
    }
}

void oscar_tick(struct oscar *o, uint32_t now_ms)
{
    o->now_ms = now_ms;
    if (o->state == OSCAR_ONLINE && now_ms - o->last_send_ms >= KEEPALIVE_MS)
        flap(o, CH_KEEPALIVE, NULL, 0);
}
