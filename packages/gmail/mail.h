/*
 * Gmail protocol and MIME engine interface (imap.c, smtp.c, mime.c).
 * Pure state machines with no socket or OS dependencies so they can be
 * tested natively with ASan/UBSan in host/gmail-test.
 */
#ifndef GMAIL_MAIL_H
#define GMAIL_MAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMAP_DEFAULT_HOST   "imap.gmail.com"
#define IMAP_DEFAULT_PORT   993
#define SMTP_DEFAULT_HOST   "smtp.gmail.com"
#define SMTP_DEFAULT_PORT   465

#define MAIL_HOST_MAX       64
#define MAIL_USER_MAX       96
#define MAIL_PASS_MAX       64
#define MAIL_ADDR_MAX       96
#define MAIL_NAME_MAX       64
#define MAIL_SUBJ_MAX       120
#define MAIL_DATE_MAX       32
#define MAIL_MSGID_MAX      96
#define MAIL_SNIPPET_MAX    96
#define MAIL_BODY_MAX       12288
#define MAIL_FETCH_MAX      32768
#define MAIL_RAW_OUT_MAX    16384
#define MAIL_MAX_MSGS       48
#define MAIL_MAX_ATTS       4
#define MAIL_MAX_RCPTS      8
#define MAIL_MBOX_PATH_MAX  64

enum {
    MBOX_INBOX = 0,
    MBOX_STARRED,
    MBOX_SENT,
    MBOX_DRAFTS,
    MBOX_ALL,
    MBOX_SPAM,
    MBOX_TRASH,
    MBOX_COUNT
};

enum {
    MAIL_F_SEEN     = 1 << 0,
    MAIL_F_FLAGGED  = 1 << 1,
    MAIL_F_ANSWERED = 1 << 2,
    MAIL_F_DRAFT    = 1 << 3,
    MAIL_F_HAS_ATT  = 1 << 4,
};

struct mail_att {
    char name[48];
    char type[32];
    uint32_t size;
};

struct mail_summary {
    uint32_t uid;
    uint8_t flags;
    char from_name[MAIL_NAME_MAX];
    char from_addr[MAIL_ADDR_MAX];
    char to[MAIL_ADDR_MAX];
    char cc[MAIL_ADDR_MAX];
    char subject[MAIL_SUBJ_MAX];
    char date[MAIL_DATE_MAX];
    char msgid[MAIL_MSGID_MAX];
    char snippet[MAIL_SNIPPET_MAX];
};

struct mail_message {
    struct mail_summary hdr;
    char reply_to[MAIL_ADDR_MAX];
    char references[MAIL_MSGID_MAX * 2];
    char body[MAIL_BODY_MAX];
    struct mail_att atts[MAIL_MAX_ATTS];
    int natts;
};

/* ---- mime.c: RFC 5322, RFC 2045-2047, and HTML to Latin-1 --------------- */

size_t mime_base64_encode(char *out, size_t out_cap, const void *in, size_t in_len);
size_t mime_base64_decode(uint8_t *out, size_t out_cap, const char *in, size_t in_len);
size_t mime_qp_decode(uint8_t *out, size_t out_cap, const char *in, size_t in_len, bool header_underscores);

void mime_to_latin1(char *out, size_t out_cap, const uint8_t *in, size_t in_len, const char *charset);
void mime_decode_header(char *out, size_t out_cap, const char *in);
void mime_html_to_text(char *out, size_t out_cap, const char *in, size_t in_len);

void mime_parse_addr(const char *raw, char *name_out, size_t name_cap, char *addr_out, size_t addr_cap);
int mime_extract_addrs(const char *list, char out[][MAIL_ADDR_MAX], int max_addrs);
void mime_parse_date(const char *raw, char *out, size_t out_cap);
void mime_reply_subject(const char *subj, bool forward, char *out, size_t out_cap);

void mime_parse_headers(const char *raw, size_t len, struct mail_summary *sum,
                        char *reply_to, size_t rt_cap, char *refs, size_t refs_cap);
void mime_parse_message(const char *raw, size_t len, struct mail_message *msg);

size_t mime_build_message(char *out, size_t out_cap, const char *from_name, const char *from_addr,
                          const char *to, const char *cc, const char *subject,
                          const char *in_reply_to, const char *references,
                          const char *date_rfc, const char *msgid, const char *body);
size_t mime_sasl_plain(char *out, size_t out_cap, const char *user, const char *pass);

/* ---- imap.c: IMAP4rev1 client state machine ----------------------------- */

enum imap_event_type {
    IMAP_EV_PROGRESS,
    IMAP_EV_READY,
    IMAP_EV_LIST_DONE,
    IMAP_EV_MSG_DONE,
    IMAP_EV_ACTION_DONE,
    IMAP_EV_NEW_MAIL,
    IMAP_EV_ERROR,
};

struct imap_event {
    enum imap_event_type type;
    const char *text;
    uint32_t uid;
    int count;
    int fatal;
};

struct imap_ops {
    void *ctx;
    int (*send)(void *ctx, const void *buf, size_t len);
    void (*on_event)(void *ctx, const struct imap_event *ev);
};

enum imap_cmd_kind {
    ICMD_NONE = 0,
    ICMD_CAPABILITY,
    ICMD_LOGIN,
    ICMD_LIST,
    ICMD_STATUS_INBOX,
    ICMD_SELECT,
    ICMD_FETCH_LIST,
    ICMD_SEARCH,
    ICMD_FETCH_SEARCH,
    ICMD_FETCH_MSG,
    ICMD_STORE_FLAG,
    ICMD_ARCHIVE_STORE,
    ICMD_ARCHIVE_EXPUNGE,
    ICMD_TRASH_COPY,
    ICMD_TRASH_STORE,
    ICMD_TRASH_EXPUNGE,
    ICMD_APPEND_DRAFT,
    ICMD_NOOP,
    ICMD_LOGOUT,
};

struct imap {
    struct imap_ops ops;
    char user[MAIL_USER_MAX];
    char pass[MAIL_PASS_MAX];

    bool greeted, logged_in, gm_ext;
    uint32_t next_tag;
    char active_tag[16];
    enum imap_cmd_kind active_cmd;
    int target_mbox;
    uint32_t target_uid;
    char search_query[96];

    /* Mailbox server paths and counts. */
    char mbox_path[MBOX_COUNT][MAIL_MBOX_PATH_MAX];
    bool mbox_exists[MBOX_COUNT];
    int cur_mbox;
    bool in_search;
    uint32_t exists_count;
    uint32_t inbox_exists;
    uint32_t inbox_unseen;
    uint32_t last_noop_ms;

    /* Search UID result list (up to MAIL_MAX_MSGS). */
    uint32_t search_uids[MAIL_MAX_MSGS];
    int nsearch_uids;

    /* Loaded message list for the current mailbox/search (newest first). */
    struct mail_summary msgs[MAIL_MAX_MSGS];
    int nmsgs;

    /* Currently fetched full message. */
    struct mail_message cur_msg;

    /* Pending draft for APPEND. */
    char *draft_buf;
    size_t draft_len;
    bool append_sent_literal;

    /* Streaming line + literal parser state. */
    char line[2048];
    size_t line_len;
    bool in_literal;
    size_t lit_rem;
    char *lit_buf;
    size_t lit_len, lit_cap;

    /* Active untagged FETCH accumulation. */
    bool in_fetch;
    uint32_t fetch_seq;
    uint32_t fetch_uid;
    uint8_t fetch_flags;
    char fetch_idate[MAIL_DATE_MAX];
};

const char *imap_mbox_title(int mbox);
void imap_init(struct imap *im, const struct imap_ops *ops);
void imap_reset(struct imap *im);
int imap_login(struct imap *im, const char *user, const char *pass);
int imap_select(struct imap *im, int mbox);
int imap_refresh(struct imap *im);
int imap_search(struct imap *im, const char *query);
int imap_fetch_message(struct imap *im, uint32_t uid);
int imap_set_flag(struct imap *im, uint32_t uid, uint8_t flag, bool on);
int imap_archive(struct imap *im, uint32_t uid);
int imap_delete(struct imap *im, uint32_t uid);
int imap_append_draft(struct imap *im, const char *raw_msg, size_t len);
int imap_logout(struct imap *im);
void imap_feed(struct imap *im, const uint8_t *data, size_t len);
void imap_tick(struct imap *im, uint32_t now_ms);

/* ---- smtp.c: ESMTP client state machine --------------------------------- */

enum smtp_event_type {
    SMTP_EV_PROGRESS,
    SMTP_EV_SENT,
    SMTP_EV_ERROR,
};

struct smtp_event {
    enum smtp_event_type type;
    const char *text;
};

struct smtp_ops {
    void *ctx;
    int (*send)(void *ctx, const void *buf, size_t len);
    void (*on_event)(void *ctx, const struct smtp_event *ev);
};

enum smtp_phase {
    SMTP_IDLE = 0,
    SMTP_GREETING,
    SMTP_EHLO,
    SMTP_AUTH,
    SMTP_AUTH_USER,
    SMTP_AUTH_PASS,
    SMTP_MAIL_FROM,
    SMTP_RCPT_TO,
    SMTP_DATA,
    SMTP_BODY,
    SMTP_QUIT,
    SMTP_DONE,
};

struct smtp {
    struct smtp_ops ops;
    enum smtp_phase phase;
    char user[MAIL_USER_MAX];
    char pass[MAIL_PASS_MAX];
    char from[MAIL_ADDR_MAX];
    char rcpts[MAIL_MAX_RCPTS][MAIL_ADDR_MAX];
    int nrcpts, cur_rcpt;
    bool auth_login_supported;
    bool auth_plain_supported;
    char *msg_buf;
    size_t msg_len;
    char line[512];
    size_t line_len;
    char multi_text[256];
};

void smtp_init(struct smtp *sm, const struct smtp_ops *ops);
void smtp_reset(struct smtp *sm);
int smtp_start(struct smtp *sm, const char *user, const char *pass, const char *from_addr,
               const char *to, const char *cc, const char *raw_msg, size_t msg_len);
void smtp_feed(struct smtp *sm, const uint8_t *data, size_t len);

#endif
