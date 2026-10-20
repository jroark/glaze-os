/*
 * OSCAR, the AIM protocol, without any I/O of its own: FLAP framing, SNACs
 * and TLVs, BUCP (MD5) sign-on, the BOS service setup, the server-side
 * buddy list (feedbag), presence, ICBM channel 1 messages with typing
 * notification, and locate (profile and away message).
 *
 * The caller owns the socket. It hands every received byte to
 * oscar_input(), and the engine writes through ops.send and reports what
 * happened through ops.event. Sign-on takes two connections: once the login
 * server accepts the password the engine raises OSCAR_EV_REDIRECT, and the
 * caller closes the login connection, opens one to bos_host:bos_port and
 * calls oscar_bos_connected().
 *
 * Integers on the wire are big-endian. Screen names are compared
 * normalized (oscar_norm: lowercase, spaces removed).
 */
#ifndef AIM_OSCAR_H
#define AIM_OSCAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSCAR_PORT          5190
#define OSCAR_SN_MAX        32      /* screen name, including the NUL */
#define OSCAR_GROUP_NAME    32
#define OSCAR_GROUPS        24
#define OSCAR_BUDDIES       200
#define OSCAR_MSG_MAX       1024    /* plain text of one IM */
#define OSCAR_INFO_MAX      1024    /* plain text of a profile or away message */
#define OSCAR_HOST_MAX      64
#define OSCAR_COOKIE_MAX    256
#define OSCAR_IN_MAX        16384   /* largest FLAP accepted (header included) */

enum oscar_state {
    OSCAR_IDLE,
    OSCAR_LOGIN,        /* login server: hello, key, password */
    OSCAR_REDIRECTED,   /* waiting for the caller to reach the BOS server */
    OSCAR_BOS,          /* BOS: service setup until client ready */
    OSCAR_ONLINE,
    OSCAR_FAILED,
};

struct oscar_buddy {
    char name[OSCAR_SN_MAX];        /* as the server spells it */
    uint16_t gid, iid;
    bool online, away;
    uint16_t idle_min;              /* 0 = active */
    uint32_t idle_since_ms;         /* oscar time of the idle report */
};

struct oscar_group {
    char name[OSCAR_GROUP_NAME];
    uint16_t gid;
};

enum oscar_event_type {
    OSCAR_EV_PROGRESS,      /* text: a sign-on step */
    OSCAR_EV_REDIRECT,      /* connect to bos_host:bos_port */
    OSCAR_EV_ONLINE,        /* signed on */
    OSCAR_EV_FAILED,        /* text: why; the connection is useless */
    OSCAR_EV_BUDDIES,       /* the list or someone's presence changed */
    OSCAR_EV_IM,            /* name, text, flag (auto-response) */
    OSCAR_EV_TYPING,        /* name, flag: 0 stopped, 1 typed, 2 typing */
    OSCAR_EV_INFO,          /* name, text = profile, text2 = away message */
    OSCAR_EV_NOT_SENT,      /* name, text: why an IM was refused */
    OSCAR_EV_EDITED,        /* flag: 0 = the server applied a list edit */
    OSCAR_EV_SIGNED_OFF,    /* text: the server ended the session */
};

struct oscar_event {
    enum oscar_event_type type;
    const char *name;
    const char *text, *text2;
    int flag;
};

struct oscar;

struct oscar_ops {
    void *ctx;
    /* Writes all of buf; < 0 on failure, which fails the session. */
    int (*send)(void *ctx, const void *buf, size_t len);
    void (*event)(void *ctx, const struct oscar_event *ev);
};

struct oscar {
    struct oscar_ops ops;
    enum oscar_state state;
    char sn[OSCAR_SN_MAX];
    char password[64];
    uint16_t seq;
    uint32_t reqid;
    /* Partial FLAP from the socket. */
    uint8_t in[OSCAR_IN_MAX];
    size_t in_len;
    size_t skip;                /* bytes left of an oversized FLAP */
    /* From the login server. */
    char bos_host[OSCAR_HOST_MAX];
    uint16_t bos_port;
    uint8_t cookie[OSCAR_COOKIE_MAX];
    uint16_t cookie_len;
    /* Rate classes to acknowledge. */
    uint16_t rate_ids[8];
    int nrates;
    /* The buddy list, in feedbag order. */
    struct oscar_group groups[OSCAR_GROUPS];
    int ngroups;
    struct oscar_buddy buddies[OSCAR_BUDDIES];
    int nbuddies;
    bool have_list, have_root;
    bool feedbag;               /* the server offers family 0x13 */
    uint16_t max_iid, max_gid;
    int edits_pending;          /* feedbag acks still expected */
    uint32_t now_ms;            /* caller's clock, from oscar_tick */
    uint32_t last_send_ms;
    char profile[OSCAR_INFO_MAX];
    /* Recipients of recent IMs by request id, to name ICBM errors. */
    struct {
        uint32_t reqid;
        char name[OSCAR_SN_MAX];
    } sent[8];
    int sent_next;
};

void oscar_init(struct oscar *o, const struct oscar_ops *ops);
/* Starts sign-on on a fresh connection to the login server. */
void oscar_login(struct oscar *o, const char *sn, const char *password);
/* The caller connected to bos_host:bos_port after OSCAR_EV_REDIRECT. */
void oscar_bos_connected(struct oscar *o);
/* Feeds received bytes. */
void oscar_input(struct oscar *o, const void *buf, size_t len);
/* The connection closed (or failed). Reports why unless that was expected. */
void oscar_closed(struct oscar *o);
/* Called regularly with a millisecond clock; sends keepalives. */
void oscar_tick(struct oscar *o, uint32_t now_ms);

/* Online requests; each returns 0 or -1 if not online / invalid. */
int oscar_send_im(struct oscar *o, const char *to, const char *text, bool autoresp);
int oscar_send_typing(struct oscar *o, const char *to, int state);
int oscar_set_profile(struct oscar *o, const char *text);
/* Empty or NULL clears the away message. */
int oscar_set_away(struct oscar *o, const char *text);
int oscar_get_info(struct oscar *o, const char *name);
/* Feedbag edits; the result arrives as OSCAR_EV_EDITED. */
int oscar_add_buddy(struct oscar *o, const char *name, const char *group);
int oscar_remove_buddy(struct oscar *o, const char *name);
int oscar_add_group(struct oscar *o, const char *group);
int oscar_remove_group(struct oscar *o, const char *group);

struct oscar_buddy *oscar_find_buddy(struct oscar *o, const char *name);
struct oscar_group *oscar_find_group(struct oscar *o, const char *name);
/* Current idle minutes of b, counting time since the report. */
int oscar_idle_minutes(const struct oscar *o, const struct oscar_buddy *b);

/* Lowercase without spaces, for comparing screen names. */
void oscar_norm(char *out, size_t n, const char *name);
bool oscar_same_name(const char *a, const char *b);

/* md5.c */
void oscar_md5(const void *data, size_t len, uint8_t out[16]);

/* text.c */
/* AIM HTML (or charset 2's UCS-2BE) to plain Latin-1, NUL-terminated. */
void oscar_html_to_text(char *out, size_t n, const uint8_t *in, size_t len, uint16_t charset);
/* Plain text to the HTML AIM clients expect; returns the length. */
size_t oscar_text_to_html(char *out, size_t n, const char *text);

#endif
