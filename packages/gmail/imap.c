/*
 * IMAP4rev1 client state machine (RFC 3501 + Gmail X-GM-EXT-1).
 * Purely event-driven and I/O-free: callers feed incoming bytes with
 * imap_feed() and transmit outgoing commands via ops.send().
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mail.h"

#define NOOP_INTERVAL_MS    60000
#define LIT_MAX             (MAIL_FETCH_MAX + 4096)

static const char *const default_mbox_paths[MBOX_COUNT] = {
    "INBOX",
    "[Gmail]/Starred",
    "[Gmail]/Sent Mail",
    "[Gmail]/Drafts",
    "[Gmail]/All Mail",
    "[Gmail]/Spam",
    "[Gmail]/Trash",
};

static const char *const mbox_titles[MBOX_COUNT] = {
    "Inbox",
    "Starred",
    "Sent Mail",
    "Drafts",
    "All Mail",
    "Spam",
    "Trash",
};

const char *imap_mbox_title(int mbox)
{
    if (mbox < 0 || mbox >= MBOX_COUNT)
        return "Mail";
    return mbox_titles[mbox];
}

static void emit(struct imap *im, enum imap_event_type type, const char *text, uint32_t uid, int count, int fatal)
{
    if (!im->ops.on_event)
        return;
    struct imap_event ev = {
        .type = type,
        .text = text,
        .uid = uid,
        .count = count,
        .fatal = fatal,
    };
    im->ops.on_event(im->ops.ctx, &ev);
}

/* Escapes `\` and `"` for an IMAP quoted string. */
static void imap_quote(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;

    if (!out_cap)
        return;
    for (const char *p = in ? in : ""; *p && o + 2 < out_cap; p++) {
        if (*p == '\r' || *p == '\n')
            continue;
        if (*p == '\\' || *p == '"')
            out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = 0;
}

static int send_raw(struct imap *im, const void *buf, size_t len)
{
    if (!im->ops.send)
        return -1;
    return im->ops.send(im->ops.ctx, buf, len);
}

static int send_cmd(struct imap *im, enum imap_cmd_kind kind, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int send_cmd(struct imap *im, enum imap_cmd_kind kind, const char *fmt, ...)
{
    char body[512], pkt[560];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    im->next_tag++;
    snprintf(im->active_tag, sizeof(im->active_tag), "A%03u", im->next_tag);
    im->active_cmd = kind;

    int n = snprintf(pkt, sizeof(pkt), "%s %s\r\n", im->active_tag, body);
    if (n <= 0 || (size_t)n >= sizeof(pkt))
        return -1;
    return send_raw(im, pkt, (size_t)n);
}

void imap_init(struct imap *im, const struct imap_ops *ops)
{
    memset(im, 0, sizeof(*im));
    if (ops)
        im->ops = *ops;
    for (int i = 0; i < MBOX_COUNT; i++) {
        strlcpy(im->mbox_path[i], default_mbox_paths[i], sizeof(im->mbox_path[i]));
        im->mbox_exists[i] = true;
    }
}

void imap_reset(struct imap *im)
{
    struct imap_ops ops = im->ops;
    free(im->lit_buf);
    free(im->draft_buf);
    imap_init(im, &ops);
}

int imap_login(struct imap *im, const char *user, const char *pass)
{
    strlcpy(im->user, user ? user : "", sizeof(im->user));
    strlcpy(im->pass, pass ? pass : "", sizeof(im->pass));
    im->greeted = false;
    im->logged_in = false;
    emit(im, IMAP_EV_PROGRESS, "Waiting for server...", 0, 0, 0);
    return 0;
}

static int start_select(struct imap *im, int mbox)
{
    char qmbox[MAIL_MBOX_PATH_MAX * 2];

    if (mbox < 0 || mbox >= MBOX_COUNT)
        return -1;
    im->target_mbox = mbox;
    im->cur_mbox = mbox;
    im->in_search = false;
    im->exists_count = 0;
    im->nmsgs = 0;
    imap_quote(im->mbox_path[mbox], qmbox, sizeof(qmbox));
    char prog[64];
    snprintf(prog, sizeof(prog), "Opening %s...", imap_mbox_title(mbox));
    emit(im, IMAP_EV_PROGRESS, prog, 0, 0, 0);
    return send_cmd(im, ICMD_SELECT, "SELECT \"%s\"", qmbox);
}

int imap_select(struct imap *im, int mbox)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE)
        return -1;
    return start_select(im, mbox);
}

int imap_refresh(struct imap *im)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE)
        return -1;
    if (im->in_search && im->search_query[0])
        return imap_search(im, im->search_query);
    return start_select(im, im->cur_mbox);
}

int imap_search(struct imap *im, const char *query)
{
    char q[192];

    if (!im->logged_in || im->active_cmd != ICMD_NONE || !query || !*query)
        return -1;
    strlcpy(im->search_query, query, sizeof(im->search_query));
    im->in_search = true;
    im->nsearch_uids = 0;
    im->nmsgs = 0;
    imap_quote(query, q, sizeof(q));
    emit(im, IMAP_EV_PROGRESS, "Searching messages...", 0, 0, 0);
    if (im->gm_ext)
        return send_cmd(im, ICMD_SEARCH, "UID SEARCH X-GM-RAW \"%s\"", q);
    return send_cmd(im, ICMD_SEARCH, "UID SEARCH OR SUBJECT \"%s\" FROM \"%s\"", q, q);
}

int imap_fetch_message(struct imap *im, uint32_t uid)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE || !uid)
        return -1;
    im->target_uid = uid;
    memset(&im->cur_msg, 0, sizeof(im->cur_msg));
    im->cur_msg.hdr.uid = uid;
    emit(im, IMAP_EV_PROGRESS, "Loading message...", uid, 0, 0);
    /* Fetch up to MAIL_FETCH_MAX bytes and mark the message \Seen on the server. */
    return send_cmd(im, ICMD_FETCH_MSG, "UID FETCH %u (UID FLAGS INTERNALDATE BODY[]<0.%u>)",
                    uid, (unsigned)MAIL_FETCH_MAX);
}

int imap_set_flag(struct imap *im, uint32_t uid, uint8_t flag, bool on)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE || !uid)
        return -1;
    const char *fname = (flag & MAIL_F_FLAGGED) ? "\\Flagged" : "\\Seen";
    im->target_uid = uid;
    for (int i = 0; i < im->nmsgs; i++) {
        if (im->msgs[i].uid == uid) {
            if (on)
                im->msgs[i].flags |= flag;
            else
                im->msgs[i].flags &= (uint8_t)~flag;
            if ((flag & MAIL_F_SEEN) && im->cur_mbox == MBOX_INBOX && !im->in_search) {
                if (on && im->inbox_unseen > 0)
                    im->inbox_unseen--;
                else if (!on)
                    im->inbox_unseen++;
            }
            break;
        }
    }
    if (im->cur_msg.hdr.uid == uid) {
        if (on)
            im->cur_msg.hdr.flags |= flag;
        else
            im->cur_msg.hdr.flags &= (uint8_t)~flag;
    }
    return send_cmd(im, ICMD_STORE_FLAG, "UID STORE %u %cFLAGS.SILENT (%s)",
                    uid, on ? '+' : '-', fname);
}

static void remove_local_uid(struct imap *im, uint32_t uid)
{
    for (int i = 0; i < im->nmsgs; i++) {
        if (im->msgs[i].uid == uid) {
            if (!(im->msgs[i].flags & MAIL_F_SEEN) && im->cur_mbox == MBOX_INBOX && im->inbox_unseen > 0)
                im->inbox_unseen--;
            memmove(&im->msgs[i], &im->msgs[i + 1], (size_t)(im->nmsgs - i - 1) * sizeof(im->msgs[0]));
            im->nmsgs--;
            break;
        }
    }
}

int imap_archive(struct imap *im, uint32_t uid)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE || !uid)
        return -1;
    im->target_uid = uid;
    remove_local_uid(im, uid);
    emit(im, IMAP_EV_PROGRESS, "Archiving...", uid, 0, 0);
    return send_cmd(im, ICMD_ARCHIVE_STORE, "UID STORE %u +FLAGS.SILENT (\\Deleted)", uid);
}

int imap_delete(struct imap *im, uint32_t uid)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE || !uid)
        return -1;
    im->target_uid = uid;
    remove_local_uid(im, uid);
    emit(im, IMAP_EV_PROGRESS, "Moving to Trash...", uid, 0, 0);
    if (im->cur_mbox != MBOX_TRASH && im->cur_mbox != MBOX_SPAM && im->mbox_path[MBOX_TRASH][0]) {
        char qtrash[MAIL_MBOX_PATH_MAX * 2];
        imap_quote(im->mbox_path[MBOX_TRASH], qtrash, sizeof(qtrash));
        return send_cmd(im, ICMD_TRASH_COPY, "UID COPY %u \"%s\"", uid, qtrash);
    }
    return send_cmd(im, ICMD_TRASH_STORE, "UID STORE %u +FLAGS.SILENT (\\Deleted)", uid);
}

int imap_append_draft(struct imap *im, const char *raw_msg, size_t len)
{
    char qdrafts[MAIL_MBOX_PATH_MAX * 2];

    if (!im->logged_in || im->active_cmd != ICMD_NONE || !raw_msg || !len)
        return -1;
    free(im->draft_buf);
    im->draft_buf = malloc(len);
    if (!im->draft_buf)
        return -1;
    memcpy(im->draft_buf, raw_msg, len);
    im->draft_len = len;
    im->append_sent_literal = false;
    imap_quote(im->mbox_path[MBOX_DRAFTS], qdrafts, sizeof(qdrafts));
    emit(im, IMAP_EV_PROGRESS, "Saving draft...", 0, 0, 0);
    return send_cmd(im, ICMD_APPEND_DRAFT, "APPEND \"%s\" (\\Draft \\Seen) {%u}", qdrafts, (unsigned)len);
}

int imap_logout(struct imap *im)
{
    if (im->active_cmd != ICMD_NONE)
        return -1;
    return send_cmd(im, ICMD_LOGOUT, "LOGOUT");
}

void imap_tick(struct imap *im, uint32_t now_ms)
{
    if (!im->logged_in || im->active_cmd != ICMD_NONE)
        return;
    if (!im->last_noop_ms) {
        im->last_noop_ms = now_ms;
        return;
    }
    if (now_ms - im->last_noop_ms >= NOOP_INTERVAL_MS) {
        im->last_noop_ms = now_ms;
        send_cmd(im, ICMD_NOOP, "NOOP");
    }
}

/* ---- Parsing IMAP responses --------------------------------------------- */

static bool has_token_ci(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (!strncasecmp(p, needle, nlen)) {
            char before = (p == hay) ? ' ' : p[-1];
            char after = p[nlen];
            if ((isspace((unsigned char)before) || before == '(') &&
                (!after || isspace((unsigned char)after) || after == ')'))
                return true;
        }
    }
    return false;
}

static void parse_list_line(struct imap *im, const char *line)
{
    /* Format: * LIST (<attrs>) "<delim>" "<name>" (or unquoted name) */
    const char *lp = strchr(line, '(');
    const char *rp = lp ? strchr(lp + 1, ')') : NULL;
    if (!lp || !rp)
        return;

    char attrs[192];
    size_t alen = (size_t)(rp - (lp + 1));
    if (alen >= sizeof(attrs))
        alen = sizeof(attrs) - 1;
    memcpy(attrs, lp + 1, alen);
    attrs[alen] = 0;

    if (has_token_ci(attrs, "\\Noselect"))
        return;

    /* Skip delimiter after ) */
    const char *p = rp + 1;
    while (*p == ' ')
        p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        if (*p == '"')
            p++;
    } else {
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;

    char name[MAIL_MBOX_PATH_MAX];
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o + 1 < sizeof(name)) {
            if (*p == '\\' && p[1])
                p++;
            name[o++] = *p++;
        }
    } else {
        while (*p && *p != '\r' && *p != '\n' && o + 1 < sizeof(name))
            name[o++] = *p++;
    }
    name[o] = 0;
    if (!name[0])
        return;

    const char *slash = strrchr(name, '/');
    const char *leaf = slash ? slash + 1 : name;

    if (!strcasecmp(name, "INBOX")) {
        strlcpy(im->mbox_path[MBOX_INBOX], name, MAIL_MBOX_PATH_MAX);
    } else if (has_token_ci(attrs, "\\Flagged") || has_token_ci(attrs, "\\Starred") ||
               !strcasecmp(leaf, "Starred")) {
        strlcpy(im->mbox_path[MBOX_STARRED], name, MAIL_MBOX_PATH_MAX);
    } else if (has_token_ci(attrs, "\\Sent") || !strcasecmp(leaf, "Sent Mail") || !strcasecmp(leaf, "Sent")) {
        strlcpy(im->mbox_path[MBOX_SENT], name, MAIL_MBOX_PATH_MAX);
    } else if (has_token_ci(attrs, "\\Drafts") || !strcasecmp(leaf, "Drafts")) {
        strlcpy(im->mbox_path[MBOX_DRAFTS], name, MAIL_MBOX_PATH_MAX);
    } else if (has_token_ci(attrs, "\\All") || has_token_ci(attrs, "\\Archive") ||
               !strcasecmp(leaf, "All Mail") || !strcasecmp(leaf, "Archive")) {
        strlcpy(im->mbox_path[MBOX_ALL], name, MAIL_MBOX_PATH_MAX);
    } else if (has_token_ci(attrs, "\\Junk") || has_token_ci(attrs, "\\Spam") ||
               !strcasecmp(leaf, "Spam") || !strcasecmp(leaf, "Junk")) {
        strlcpy(im->mbox_path[MBOX_SPAM], name, MAIL_MBOX_PATH_MAX);
    } else if (has_token_ci(attrs, "\\Trash") || !strcasecmp(leaf, "Trash") ||
               !strcasecmp(leaf, "Deleted Items")) {
        strlcpy(im->mbox_path[MBOX_TRASH], name, MAIL_MBOX_PATH_MAX);
    }
}

static void parse_fetch_tokens(struct imap *im, const char *s)
{
    for (const char *p = s; *p;) {
        if ((p == s || isspace((unsigned char)p[-1]) || p[-1] == '(') && !strncasecmp(p, "UID ", 4)) {
            p += 4;
            while (*p == ' ')
                p++;
            im->fetch_uid = (uint32_t)strtoul(p, (char **)&p, 10);
            continue;
        }
        if ((p == s || isspace((unsigned char)p[-1]) || p[-1] == '(') && !strncasecmp(p, "FLAGS ", 6)) {
            p += 6;
            while (*p == ' ')
                p++;
            if (*p == '(') {
                const char *rp = strchr(p + 1, ')');
                if (rp) {
                    char fl[128];
                    size_t flen = (size_t)(rp - (p + 1));
                    if (flen >= sizeof(fl))
                        flen = sizeof(fl) - 1;
                    memcpy(fl, p + 1, flen);
                    fl[flen] = 0;
                    if (has_token_ci(fl, "\\Seen"))
                        im->fetch_flags |= MAIL_F_SEEN;
                    if (has_token_ci(fl, "\\Flagged"))
                        im->fetch_flags |= MAIL_F_FLAGGED;
                    if (has_token_ci(fl, "\\Answered"))
                        im->fetch_flags |= MAIL_F_ANSWERED;
                    if (has_token_ci(fl, "\\Draft"))
                        im->fetch_flags |= MAIL_F_DRAFT;
                    p = rp + 1;
                    continue;
                }
            }
        }
        if ((p == s || isspace((unsigned char)p[-1]) || p[-1] == '(') && !strncasecmp(p, "INTERNALDATE ", 13)) {
            p += 13;
            while (*p == ' ')
                p++;
            if (*p == '"') {
                p++;
                const char *q = strchr(p, '"');
                if (q) {
                    char raw_d[64];
                    size_t dlen = (size_t)(q - p);
                    if (dlen >= sizeof(raw_d))
                        dlen = sizeof(raw_d) - 1;
                    memcpy(raw_d, p, dlen);
                    raw_d[dlen] = 0;
                    mime_parse_date(raw_d, im->fetch_idate, sizeof(im->fetch_idate));
                    p = q + 1;
                    continue;
                }
            }
        }
        p++;
    }
}

static void finish_fetch_item(struct imap *im)
{
    if (!im->in_fetch)
        return;
    im->in_fetch = false;

    uint32_t uid = im->fetch_uid ? im->fetch_uid : im->fetch_seq;
    if (!uid)
        return;

    if (im->active_cmd == ICMD_FETCH_MSG) {
        if (uid == im->target_uid || !im->target_uid) {
            im->cur_msg.hdr.uid = uid;
            im->cur_msg.hdr.flags = im->fetch_flags | MAIL_F_SEEN;
            if (im->fetch_idate[0])
                strlcpy(im->cur_msg.hdr.date, im->fetch_idate, sizeof(im->cur_msg.hdr.date));
            if (im->lit_buf && im->lit_len > 0)
                mime_parse_message(im->lit_buf, im->lit_len, &im->cur_msg);
            /* Sync summary in im->msgs[] if present. */
            for (int i = 0; i < im->nmsgs; i++) {
                if (im->msgs[i].uid == uid) {
                    if (!(im->msgs[i].flags & MAIL_F_SEEN) && im->cur_mbox == MBOX_INBOX && im->inbox_unseen > 0)
                        im->inbox_unseen--;
                    im->msgs[i].flags |= MAIL_F_SEEN | (im->cur_msg.hdr.flags & MAIL_F_HAS_ATT);
                    if (im->cur_msg.hdr.snippet[0])
                        strlcpy(im->msgs[i].snippet, im->cur_msg.hdr.snippet, sizeof(im->msgs[i].snippet));
                    break;
                }
            }
        }
    } else if (im->active_cmd == ICMD_FETCH_LIST || im->active_cmd == ICMD_FETCH_SEARCH) {
        if (im->nmsgs < MAIL_MAX_MSGS) {
            struct mail_summary *s = &im->msgs[im->nmsgs++];
            memset(s, 0, sizeof(*s));
            s->uid = uid;
            s->flags = im->fetch_flags;
            if (im->fetch_idate[0])
                strlcpy(s->date, im->fetch_idate, sizeof(s->date));
            if (im->lit_buf && im->lit_len > 0)
                mime_parse_headers(im->lit_buf, im->lit_len, s, NULL, 0, NULL, 0);
            if (!s->subject[0])
                strlcpy(s->subject, "(No Subject)", sizeof(s->subject));
        }
    }
    im->lit_len = 0;
}

/* Checks if `line` ends with `{<number>}`; returns true and sets *n_out. */
static bool ends_with_literal(const char *line, size_t *n_out)
{
    size_t len = strlen(line);
    if (len < 3 || line[len - 1] != '}')
        return false;
    const char *lb = strrchr(line, '{');
    if (!lb || lb + 1 >= line + len - 1)
        return false;
    const char *p = lb + 1;
    if (*p == '+')
        return false;
    size_t val = 0;
    for (; p < line + len - 1; p++) {
        if (!isdigit((unsigned char)*p))
            return false;
        val = val * 10 + (size_t)(*p - '0');
        if (val > 16 * 1024 * 1024)
            return false;
    }
    *n_out = val;
    return true;
}

static int cmp_summary_desc(const void *a, const void *b)
{
    const struct mail_summary *sa = a, *sb = b;
    if (sa->uid != sb->uid)
        return sa->uid < sb->uid ? 1 : -1;
    return 0;
}

static void start_list_fetch(struct imap *im)
{
    if (im->exists_count == 0) {
        im->nmsgs = 0;
        if (im->cur_mbox == MBOX_INBOX)
            im->inbox_unseen = 0;
        if (!im->logged_in) {
            im->logged_in = true;
            emit(im, IMAP_EV_READY, NULL, 0, 0, 0);
        } else {
            emit(im, IMAP_EV_LIST_DONE, NULL, 0, 0, 0);
        }
        return;
    }
    uint32_t hi = im->exists_count;
    uint32_t lo = hi > MAIL_MAX_MSGS ? hi - MAIL_MAX_MSGS + 1 : 1;
    send_cmd(im, ICMD_FETCH_LIST,
             "FETCH %u:%u (UID FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)])",
             lo, hi);
}

static void on_command_done(struct imap *im, enum imap_cmd_kind cmd, bool ok, const char *rest)
{
    switch (cmd) {
    case ICMD_CAPABILITY:
        if (im->user[0]) {
            char qu[MAIL_USER_MAX * 2], qp[MAIL_PASS_MAX * 2];
            imap_quote(im->user, qu, sizeof(qu));
            imap_quote(im->pass, qp, sizeof(qp));
            emit(im, IMAP_EV_PROGRESS, "Signing in...", 0, 0, 0);
            send_cmd(im, ICMD_LOGIN, "LOGIN \"%s\" \"%s\"", qu, qp);
        }
        break;

    case ICMD_LOGIN:
        if (!ok) {
            emit(im, IMAP_EV_ERROR, "Sign-in failed. Check your email and App Password.", 0, 0, 1);
            return;
        }
        emit(im, IMAP_EV_PROGRESS, "Loading mailboxes...", 0, 0, 0);
        send_cmd(im, ICMD_LIST, "LIST \"\" \"*\"");
        break;

    case ICMD_LIST:
        send_cmd(im, ICMD_STATUS_INBOX, "STATUS \"INBOX\" (MESSAGES UNSEEN)");
        break;

    case ICMD_STATUS_INBOX:
        start_select(im, MBOX_INBOX);
        break;

    case ICMD_SELECT:
        if (!ok) {
            emit(im, IMAP_EV_ERROR, "Mailbox is not available on this server.", 0, 0, 0);
            return;
        }
        start_list_fetch(im);
        break;

    case ICMD_FETCH_LIST:
    case ICMD_FETCH_SEARCH:
        if (im->nmsgs > 1)
            qsort(im->msgs, (size_t)im->nmsgs, sizeof(im->msgs[0]), cmp_summary_desc);
        if (im->cur_mbox == MBOX_INBOX && !im->in_search) {
            uint32_t unseen = 0;
            for (int i = 0; i < im->nmsgs; i++)
                if (!(im->msgs[i].flags & MAIL_F_SEEN))
                    unseen++;
            if (im->exists_count <= MAIL_MAX_MSGS || unseen > im->inbox_unseen)
                im->inbox_unseen = unseen;
        }
        if (!im->logged_in) {
            im->logged_in = true;
            emit(im, IMAP_EV_READY, NULL, 0, (int)im->inbox_unseen, 0);
        } else {
            emit(im, IMAP_EV_LIST_DONE, NULL, 0, (int)im->inbox_unseen, 0);
        }
        break;

    case ICMD_SEARCH:
        if (!ok || im->nsearch_uids == 0) {
            im->nmsgs = 0;
            emit(im, IMAP_EV_LIST_DONE, NULL, 0, (int)im->inbox_unseen, 0);
            return;
        }
        {
            char seq[384];
            size_t o = 0;
            for (int i = 0; i < im->nsearch_uids; i++) {
                int n = snprintf(seq + o, sizeof(seq) - o, "%s%u", i ? "," : "", im->search_uids[i]);
                if (n <= 0 || (size_t)n >= sizeof(seq) - o)
                    break;
                o += (size_t)n;
            }
            send_cmd(im, ICMD_FETCH_SEARCH,
                     "UID FETCH %s (UID FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)])",
                     seq);
        }
        break;

    case ICMD_FETCH_MSG:
        if (!ok) {
            emit(im, IMAP_EV_ERROR, "Could not load message from server.", im->target_uid, 0, 0);
            return;
        }
        emit(im, IMAP_EV_MSG_DONE, NULL, im->cur_msg.hdr.uid, (int)im->inbox_unseen, 0);
        break;

    case ICMD_STORE_FLAG:
        emit(im, IMAP_EV_ACTION_DONE, NULL, im->target_uid, (int)im->inbox_unseen, 0);
        break;

    case ICMD_ARCHIVE_STORE:
        send_cmd(im, ICMD_ARCHIVE_EXPUNGE, "EXPUNGE");
        break;

    case ICMD_ARCHIVE_EXPUNGE:
        emit(im, IMAP_EV_ACTION_DONE, "Archived", im->target_uid, (int)im->inbox_unseen, 0);
        break;

    case ICMD_TRASH_COPY:
        send_cmd(im, ICMD_TRASH_STORE, "UID STORE %u +FLAGS.SILENT (\\Deleted)", im->target_uid);
        break;

    case ICMD_TRASH_STORE:
        send_cmd(im, ICMD_TRASH_EXPUNGE, "EXPUNGE");
        break;

    case ICMD_TRASH_EXPUNGE:
        emit(im, IMAP_EV_ACTION_DONE, "Deleted", im->target_uid, (int)im->inbox_unseen, 0);
        break;

    case ICMD_APPEND_DRAFT:
        free(im->draft_buf);
        im->draft_buf = NULL;
        im->draft_len = 0;
        if (ok)
            emit(im, IMAP_EV_ACTION_DONE, "Draft saved", 0, (int)im->inbox_unseen, 0);
        else
            emit(im, IMAP_EV_ERROR, "Could not save draft on server.", 0, 0, 0);
        break;

    case ICMD_NOOP:
    case ICMD_LOGOUT:
    case ICMD_NONE:
        (void)rest;
        break;
    }
}

static void handle_line(struct imap *im, const char *line)
{
    size_t lit_n = 0;
    bool has_lit = ends_with_literal(line, &lit_n);

    if (im->in_fetch) {
        parse_fetch_tokens(im, line);
        if (has_lit) {
            im->in_literal = true;
            im->lit_rem = lit_n;
            im->lit_len = 0;
            return;
        }
        finish_fetch_item(im);
        return;
    }

    /* Command continuation request: + Ready for literal data (used by APPEND). */
    if (line[0] == '+' && (line[1] == ' ' || !line[1])) {
        if (im->active_cmd == ICMD_APPEND_DRAFT && im->draft_buf && !im->append_sent_literal) {
            im->append_sent_literal = true;
            send_raw(im, im->draft_buf, im->draft_len);
            send_raw(im, "\r\n", 2);
        }
        return;
    }

    /* Untagged responses: * ... */
    if (line[0] == '*' && line[1] == ' ') {
        const char *p = line + 2;

        if (!im->greeted && (!strncasecmp(p, "OK", 2) || !strncasecmp(p, "PREAUTH", 7))) {
            im->greeted = true;
            if (strstr(p, "X-GM-EXT-1"))
                im->gm_ext = true;
            send_cmd(im, ICMD_CAPABILITY, "CAPABILITY");
            return;
        }
        if (!strncasecmp(p, "CAPABILITY", 10)) {
            if (strstr(p, "X-GM-EXT-1"))
                im->gm_ext = true;
            return;
        }
        if (!strncasecmp(p, "LIST ", 5)) {
            parse_list_line(im, p);
            return;
        }
        if (!strncasecmp(p, "STATUS ", 7)) {
            const char *u = strstr(p, "UNSEEN ");
            if (u)
                im->inbox_unseen = (uint32_t)strtoul(u + 7, NULL, 10);
            const char *m = strstr(p, "MESSAGES ");
            if (m)
                im->inbox_exists = (uint32_t)strtoul(m + 9, NULL, 10);
            return;
        }
        if (!strncasecmp(p, "SEARCH", 6)) {
            p += 6;
            uint32_t all[256];
            int nall = 0;
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!isdigit((unsigned char)*p))
                    break;
                uint32_t u = (uint32_t)strtoul(p, (char **)&p, 10);
                if (u && nall < 256)
                    all[nall++] = u;
            }
            /* Keep the newest MAIL_MAX_MSGS UIDs. */
            int start = nall > MAIL_MAX_MSGS ? nall - MAIL_MAX_MSGS : 0;
            im->nsearch_uids = 0;
            for (int i = start; i < nall && im->nsearch_uids < MAIL_MAX_MSGS; i++)
                im->search_uids[im->nsearch_uids++] = all[i];
            return;
        }

        /* Numeric untagged responses: * <num> EXISTS | EXPUNGE | FETCH (...) */
        if (isdigit((unsigned char)*p)) {
            char *after = NULL;
            uint32_t num = (uint32_t)strtoul(p, &after, 10);
            while (after && *after == ' ')
                after++;
            if (after && !strncasecmp(after, "EXISTS", 6)) {
                uint32_t prev = im->exists_count;
                im->exists_count = num;
                if (im->cur_mbox == MBOX_INBOX) {
                    if (im->logged_in && num > prev && im->active_cmd == ICMD_NOOP) {
                        uint32_t delta = num - prev;
                        im->inbox_unseen += delta;
                        emit(im, IMAP_EV_NEW_MAIL, NULL, 0, (int)delta, 0);
                    }
                    im->inbox_exists = num;
                }
                return;
            }
            if (after && !strncasecmp(after, "FETCH ", 6)) {
                im->in_fetch = true;
                im->fetch_seq = num;
                im->fetch_uid = 0;
                im->fetch_flags = 0;
                im->fetch_idate[0] = 0;
                im->lit_len = 0;
                parse_fetch_tokens(im, after + 6);
                if (has_lit) {
                    im->in_literal = true;
                    im->lit_rem = lit_n;
                } else {
                    finish_fetch_item(im);
                }
                return;
            }
        }
        return;
    }

    /* Tagged completion response: <tag> OK|NO|BAD ... */
    size_t tlen = strlen(im->active_tag);
    if (tlen > 0 && !strncmp(line, im->active_tag, tlen) && line[tlen] == ' ') {
        const char *st = line + tlen + 1;
        bool ok = !strncasecmp(st, "OK", 2);
        if (ok && strstr(st, "X-GM-EXT-1"))
            im->gm_ext = true;
        enum imap_cmd_kind done_cmd = im->active_cmd;
        im->active_cmd = ICMD_NONE;
        im->active_tag[0] = 0;
        on_command_done(im, done_cmd, ok, st);
    }
}

void imap_feed(struct imap *im, const uint8_t *data, size_t len)
{
    size_t i = 0;

    while (i < len) {
        if (im->in_literal) {
            size_t avail = len - i;
            size_t take = avail < im->lit_rem ? avail : im->lit_rem;
            /* Buffer up to LIT_MAX bytes of the literal; discard any excess safely. */
            if (im->lit_len < LIT_MAX) {
                size_t keep = take;
                if (im->lit_len + keep > LIT_MAX)
                    keep = LIT_MAX - im->lit_len;
                if (im->lit_len + keep + 1 > im->lit_cap) {
                    size_t ncap = im->lit_cap ? im->lit_cap * 2 : 1024;
                    while (ncap < im->lit_len + keep + 1)
                        ncap *= 2;
                    if (ncap > LIT_MAX + 1)
                        ncap = LIT_MAX + 1;
                    char *nb = realloc(im->lit_buf, ncap);
                    if (nb) {
                        im->lit_buf = nb;
                        im->lit_cap = ncap;
                    } else {
                        keep = 0;
                    }
                }
                if (keep > 0 && im->lit_buf) {
                    memcpy(im->lit_buf + im->lit_len, data + i, keep);
                    im->lit_len += keep;
                    im->lit_buf[im->lit_len] = 0;
                }
            }
            i += take;
            im->lit_rem -= take;
            if (im->lit_rem == 0)
                im->in_literal = false;
            continue;
        }

        uint8_t c = data[i++];
        if (c == '\r')
            continue;
        if (c == '\n') {
            im->line[im->line_len] = 0;
            im->line_len = 0;
            handle_line(im, im->line);
        } else if (im->line_len + 1 < sizeof(im->line)) {
            im->line[im->line_len++] = (char)c;
        }
    }
}
