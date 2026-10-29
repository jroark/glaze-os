/*
 * Native tests and live/mock tooling for the Gmail package's MIME, IMAP4rev1,
 * and ESMTP engines (packages/gmail/mime.c, imap.c, smtp.c):
 *
 *   gmail-test                                 unit tests (fed byte by byte)
 *   gmail-test --live imap_host imap_port smtp_host smtp_port user pass [to subj body]
 *                                              connect to a real server
 *   gmail-test --serve imap_port smtp_port     local IMAP + SMTP fixture server
 */

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include "../packages/gmail/mail.h"

/* ---- wolfSSL hooks for --live ------------------------------------------- */

int glaze_tls_seed(unsigned char *out, unsigned int n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t r = fread(out, 1, n, f);
        fclose(f);
        if (r == n)
            return 0;
    }
    for (unsigned int i = 0; i < n; i++)
        out[i] = (unsigned char)(i * 31 + 17);
    return 0;
}

time_t glaze_tls_time(time_t *t)
{
    time_t now = time(NULL);
    if (t)
        *t = now;
    return now;
}

/* ---- Test capture harnesses --------------------------------------------- */

static char sent_buf[65536];
static size_t sent_len;

#define MAX_EV 64
static struct {
    enum imap_event_type type;
    char text[128];
    uint32_t uid;
    int count;
    int fatal;
} imap_evs[MAX_EV];
static int nimap_evs;

static struct {
    enum smtp_event_type type;
    char text[128];
} smtp_evs[MAX_EV];
static int nsmtp_evs;

static void reset_cap(void)
{
    sent_len = 0;
    sent_buf[0] = 0;
    nimap_evs = 0;
    nsmtp_evs = 0;
}

static int cap_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    assert(sent_len + len < sizeof(sent_buf));
    memcpy(sent_buf + sent_len, buf, len);
    sent_len += len;
    sent_buf[sent_len] = 0;
    return 0;
}

static void cap_imap_ev(void *ctx, const struct imap_event *ev)
{
    (void)ctx;
    assert(nimap_evs < MAX_EV);
    imap_evs[nimap_evs].type = ev->type;
    snprintf(imap_evs[nimap_evs].text, sizeof(imap_evs[0].text), "%s", ev->text ? ev->text : "");
    imap_evs[nimap_evs].uid = ev->uid;
    imap_evs[nimap_evs].count = ev->count;
    imap_evs[nimap_evs].fatal = ev->fatal;
    nimap_evs++;
}

static void cap_smtp_ev(void *ctx, const struct smtp_event *ev)
{
    (void)ctx;
    assert(nsmtp_evs < MAX_EV);
    smtp_evs[nsmtp_evs].type = ev->type;
    snprintf(smtp_evs[nsmtp_evs].text, sizeof(smtp_evs[0].text), "%s", ev->text ? ev->text : "");
    nsmtp_evs++;
}

static void feed_imap_str(struct imap *im, const char *s)
{
    /* Feed byte by byte to exercise streaming line and literal boundaries. */
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        imap_feed(im, (const uint8_t *)s + i, 1);
}

static void feed_smtp_str(struct smtp *sm, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        smtp_feed(sm, (const uint8_t *)s + i, 1);
}

/* ---- Unit tests: MIME --------------------------------------------------- */

static void test_mime(void)
{
    char b64[128];
    uint8_t dec[128];

    size_t elen = mime_base64_encode(b64, sizeof(b64), "Hello, Glaze Mail!", 18);
    assert(elen == 24 && !strcmp(b64, "SGVsbG8sIEdsYXplIE1haWwh"));
    const char *b64_in = "SGVsbG8s\r\n IEdsYXplIE1haWwh";
    size_t dlen = mime_base64_decode(dec, sizeof(dec), b64_in, strlen(b64_in));
    assert(dlen == 18 && !memcmp(dec, "Hello, Glaze Mail!", 18));

    const char *qp_in = "Caf=C3=A9 =E2=80=94 soft=\r\nbreak!";
    dlen = mime_qp_decode(dec, sizeof(dec), qp_in, strlen(qp_in), false);
    char latin[128];
    mime_to_latin1(latin, sizeof(latin), dec, dlen, "utf-8");
    assert(!strcmp(latin, "Caf\xe9 - softbreak!"));

    /* RFC 2047 encoded-words (adjacent B and Q words + plain text). */
    char hdr[128];
    mime_decode_header(hdr, sizeof(hdr),
                       "=?UTF-8?B?QWxpY2Ug?= =?UTF-8?Q?Br=C3=BChl_Jr.?= <alice@gmail.com>");
    assert(!strcmp(hdr, "Alice Br\xfchl Jr. <alice@gmail.com>"));

    char name[MAIL_NAME_MAX], addr[MAIL_ADDR_MAX];
    mime_parse_addr("\"Casio Team\" <team@glaze-os.com>", name, sizeof(name), addr, sizeof(addr));
    assert(!strcmp(name, "Casio Team") && !strcmp(addr, "team@glaze-os.com"));
    mime_parse_addr("plain@example.org (Plain User)", name, sizeof(name), addr, sizeof(addr));
    assert(!strcmp(name, "Plain User") && !strcmp(addr, "plain@example.org"));

    char addrs[4][MAIL_ADDR_MAX];
    int naddrs = mime_extract_addrs("\"Alice, A.\" <a@ex.com>, b@ex.com; <c@ex.com>", addrs, 4);
    assert(naddrs == 3);
    assert(!strcmp(addrs[0], "a@ex.com") && !strcmp(addrs[1], "b@ex.com") && !strcmp(addrs[2], "c@ex.com"));

    char dstr[32];
    mime_parse_date("Wed, 21 Oct 2026 16:05:00 -0700", dstr, sizeof(dstr));
    assert(!strcmp(dstr, "Oct 21"));
    mime_parse_date("05-Nov-2026 09:12:00 +0000", dstr, sizeof(dstr));
    assert(!strcmp(dstr, "Nov 5"));

    char rsubj[64];
    mime_reply_subject("Re:   Project Update", false, rsubj, sizeof(rsubj));
    assert(!strcmp(rsubj, "Re: Project Update"));
    mime_reply_subject("Fwd: Project Update", true, rsubj, sizeof(rsubj));
    assert(!strcmp(rsubj, "Fwd: Project Update"));

    /* HTML to Latin-1 text conversion. */
    const char *html =
        "<html><head><title>Ignored</title><style>body{color:red}</style></head>"
        "<body><p>Hello <b>World</b> &amp; Friends&hellip;</p>"
        "<ul><li>First item</li><li>Second &mdash; item</li></ul></body></html>";
    char txt[256];
    mime_html_to_text(txt, sizeof(txt), html, strlen(html));
    assert(!strcmp(txt, "Hello World & Friends\x84\n\n\x83 First item\n\x83 Second - item"));

    /* Nested multipart/mixed + multipart/alternative + attachment message. */
    const char *raw_mime =
        "From: =?UTF-8?Q?Ren=C3=A9_Descartes?= <rene@gmail.com>\r\n"
        "To: user@gmail.com\r\n"
        "Cc: peer@gmail.com\r\n"
        "Subject: =?UTF-8?B?VGhpbmtpbmcgb24gQkUtMzAw?=\r\n"
        "Date: Tue, 20 Oct 2026 11:30:00 -0700\r\n"
        "Message-ID: <msg-101@gmail.com>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"mix-bound\"\r\n"
        "\r\n"
        "--mix-bound\r\n"
        "Content-Type: multipart/alternative; boundary=\"alt-bound\"\r\n"
        "\r\n"
        "--alt-bound\r\n"
        "Content-Type: text/plain; charset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: quoted-printable\r\n"
        "\r\n"
        "Cogito, ergo sum =E2=80=94 on a 240x320 LCD!\r\n"
        "--alt-bound\r\n"
        "Content-Type: text/html; charset=\"utf-8\"\r\n"
        "\r\n"
        "<p>HTML fallback should not override plain text</p>\r\n"
        "--alt-bound--\r\n"
        "--mix-bound\r\n"
        "Content-Type: application/pdf; name=\"manual.pdf\"\r\n"
        "Content-Disposition: attachment; filename=\"manual.pdf\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "JVBERi0xLjQK\r\n"
        "--mix-bound--\r\n";

    struct mail_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.uid = 101;
    mime_parse_message(raw_mime, strlen(raw_mime), &msg);
    assert(msg.hdr.uid == 101);
    assert(!strcmp(msg.hdr.from_name, "Ren\xe9 Descartes"));
    assert(!strcmp(msg.hdr.from_addr, "rene@gmail.com"));
    assert(!strcmp(msg.hdr.subject, "Thinking on BE-300"));
    assert(!strcmp(msg.hdr.date, "Oct 20"));
    assert(!strcmp(msg.hdr.msgid, "<msg-101@gmail.com>"));
    assert(!strcmp(msg.body, "Cogito, ergo sum - on a 240x320 LCD!"));
    assert(!strcmp(msg.hdr.snippet, "Cogito, ergo sum - on a 240x320 LCD!"));
    assert((msg.hdr.flags & MAIL_F_HAS_ATT) != 0);
    assert(msg.natts == 1 && !strcmp(msg.atts[0].name, "manual.pdf"));
}

/* ---- Unit tests: IMAP4rev1 ---------------------------------------------- */

static void test_imap(void)
{
    struct imap im;
    reset_cap();
    imap_init(&im, &(struct imap_ops){ NULL, cap_send, cap_imap_ev });
    imap_login(&im, "glaze@gmail.com", "abcd efgh ijkl mnop");

    /* Server greeting -> CAPABILITY -> LOGIN -> LIST -> STATUS -> SELECT -> FETCH */
    feed_imap_str(&im, "* OK Gimap ready for requests\r\n");
    assert(strstr(sent_buf, "A001 CAPABILITY\r\n") != NULL);

    reset_cap();
    feed_imap_str(&im, "* CAPABILITY IMAP4rev1 UNSELECT IDLE NAMESPACE X-GM-EXT-1\r\n"
                       "A001 OK Thats all she wrote!\r\n");
    assert(im.gm_ext);
    assert(strstr(sent_buf, "A002 LOGIN \"glaze@gmail.com\" \"abcd efgh ijkl mnop\"\r\n") != NULL);

    reset_cap();
    feed_imap_str(&im, "A002 OK glaze@gmail.com authenticated (Success)\r\n");
    assert(strstr(sent_buf, "A003 LIST \"\" \"*\"\r\n") != NULL);

    reset_cap();
    feed_imap_str(&im,
        "* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n"
        "* LIST (\\HasChildren \\Noselect) \"/\" \"[Gmail]\"\r\n"
        "* LIST (\\All \\HasNoChildren) \"/\" \"[Gmail]/All Mail\"\r\n"
        "* LIST (\\Drafts \\HasNoChildren) \"/\" \"[Gmail]/Drafts\"\r\n"
        "* LIST (\\Sent \\HasNoChildren) \"/\" \"[Gmail]/Sent Mail\"\r\n"
        "* LIST (\\Flagged \\HasNoChildren) \"/\" \"[Gmail]/Starred\"\r\n"
        "* LIST (\\Trash \\HasNoChildren) \"/\" \"[Gmail]/Bin\"\r\n"
        "A003 OK Success\r\n");
    assert(!strcmp(im.mbox_path[MBOX_TRASH], "[Gmail]/Bin"));
    assert(strstr(sent_buf, "A004 STATUS \"INBOX\" (MESSAGES UNSEEN)\r\n") != NULL);

    reset_cap();
    feed_imap_str(&im,
        "* STATUS \"INBOX\" (MESSAGES 2 UNSEEN 1)\r\n"
        "A004 OK Success\r\n");
    assert(im.inbox_unseen == 1);
    assert(strstr(sent_buf, "A005 SELECT \"INBOX\"\r\n") != NULL);

    reset_cap();
    feed_imap_str(&im,
        "* FLAGS (\\Answered \\Flagged \\Draft \\Deleted \\Seen)\r\n"
        "* 2 EXISTS\r\n"
        "A005 OK [READ-WRITE] INBOX selected.\r\n");
    assert(strstr(sent_buf, "A006 FETCH 1:2 (UID FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS") != NULL);

    /* Feed two FETCH responses: one with UID before the literal, one with UID after the literal (Gmail style). */
    const char *h1 = "From: Alice <alice@gmail.com>\r\nSubject: First Note\r\nDate: Mon, 19 Oct 2026 10:00:00 -0700\r\n\r\n";
    const char *h2 = "From: Bob <bob@gmail.com>\r\nSubject: Latest Update\r\nDate: Wed, 21 Oct 2026 12:00:00 -0700\r\n\r\n";
    char fetch_resp[1024];
    snprintf(fetch_resp, sizeof(fetch_resp),
             "* 1 FETCH (UID 501 FLAGS (\\Seen \\Flagged) INTERNALDATE \"19-Oct-2026 10:00:00 -0700\" "
             "BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n%s)\r\n"
             "* 2 FETCH (FLAGS () INTERNALDATE \"21-Oct-2026 12:00:00 -0700\" "
             "BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n%s UID 505)\r\n"
             "A006 OK Success\r\n",
             strlen(h1), h1, strlen(h2), h2);
    reset_cap();
    feed_imap_str(&im, fetch_resp);

    assert(im.logged_in);
    assert(im.nmsgs == 2);
    /* Sorted newest first (UID 505 at index 0, UID 501 at index 1). */
    assert(im.msgs[0].uid == 505 && !strcmp(im.msgs[0].from_name, "Bob") && !(im.msgs[0].flags & MAIL_F_SEEN));
    assert(im.msgs[1].uid == 501 && !strcmp(im.msgs[1].from_name, "Alice") && (im.msgs[1].flags & MAIL_F_FLAGGED));

    /* Fetch full message 505. */
    reset_cap();
    assert(imap_fetch_message(&im, 505) == 0);
    assert(strstr(sent_buf, "UID FETCH 505") != NULL);
    const char *full2 =
        "From: Bob <bob@gmail.com>\r\n"
        "To: glaze@gmail.com\r\n"
        "Subject: Latest Update\r\n"
        "Date: Wed, 21 Oct 2026 12:00:00 -0700\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Here is the body of the latest update!\r\n";
    snprintf(fetch_resp, sizeof(fetch_resp),
             "* 2 FETCH (FLAGS (\\Seen) INTERNALDATE \"21-Oct-2026 12:00:00 -0700\" BODY[]<0> {%zu}\r\n%s UID 505)\r\n"
             "A007 OK Success\r\n",
             strlen(full2), full2);
    feed_imap_str(&im, fetch_resp);
    assert(im.cur_msg.hdr.uid == 505);
    assert(!strcmp(im.cur_msg.body, "Here is the body of the latest update!"));
    assert((im.msgs[0].flags & MAIL_F_SEEN) != 0);
    assert(im.inbox_unseen == 0);

    /* Search via X-GM-RAW. */
    reset_cap();
    assert(imap_search(&im, "from:Alice") == 0);
    assert(strstr(sent_buf, "UID SEARCH X-GM-RAW \"from:Alice\"\r\n") != NULL);
    reset_cap();
    feed_imap_str(&im, "* SEARCH 501\r\nA008 OK Search completed\r\n");
    assert(strstr(sent_buf, "UID FETCH 501") != NULL);
    snprintf(fetch_resp, sizeof(fetch_resp),
             "* 1 FETCH (UID 501 FLAGS (\\Seen \\Flagged) BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n%s)\r\n"
             "A009 OK Success\r\n",
             strlen(h1), h1);
    feed_imap_str(&im, fetch_resp);
    assert(im.nmsgs == 1 && im.msgs[0].uid == 501);

    /* Delete (copies to [Gmail]/Bin, marks \Deleted, expunges). */
    reset_cap();
    assert(imap_delete(&im, 501) == 0);
    assert(strstr(sent_buf, "UID COPY 501 \"[Gmail]/Bin\"\r\n") != NULL);
    reset_cap();
    feed_imap_str(&im, "A010 OK Copied\r\n");
    assert(strstr(sent_buf, "UID STORE 501 +FLAGS.SILENT (\\Deleted)\r\n") != NULL);
    reset_cap();
    feed_imap_str(&im, "A011 OK Stored\r\n");
    assert(strstr(sent_buf, "EXPUNGE\r\n") != NULL);
    feed_imap_str(&im, "* 1 EXPUNGE\r\nA012 OK Expunged\r\n");
    assert(im.nmsgs == 0);

    imap_reset(&im);
}

/* ---- Unit tests: ESMTP -------------------------------------------------- */

static void test_smtp(void)
{
    struct smtp sm;
    char raw[1024];

    size_t rlen = mime_build_message(raw, sizeof(raw), "Glaze User", "glaze@gmail.com",
                                     "bob@gmail.com", "cc@gmail.com", "Hello via SMTP",
                                     NULL, NULL, "Wed, 21 Oct 2026 16:00:00 -0700",
                                     "<test-1@glaze.local>", "Line 1\n.Dot line\nLine 3");
    reset_cap();
    smtp_init(&sm, &(struct smtp_ops){ NULL, cap_send, cap_smtp_ev });
    assert(smtp_start(&sm, "glaze@gmail.com", "secretpass", "glaze@gmail.com",
                      "bob@gmail.com", "cc@gmail.com", raw, rlen) == 0);

    feed_smtp_str(&sm, "220 smtp.gmail.com ESMTP ready\r\n");
    assert(!strcmp(sent_buf, "EHLO glaze.local\r\n"));

    reset_cap();
    feed_smtp_str(&sm, "250-smtp.gmail.com at your service\r\n"
                       "250-SIZE 35882577\r\n"
                       "250 AUTH LOGIN PLAIN XOAUTH2\r\n");
    assert(!strncmp(sent_buf, "AUTH PLAIN ", 11));

    reset_cap();
    feed_smtp_str(&sm, "235 2.7.0 Accepted\r\n");
    assert(!strcmp(sent_buf, "MAIL FROM:<glaze@gmail.com>\r\n"));

    reset_cap();
    feed_smtp_str(&sm, "250 2.1.0 OK\r\n");
    assert(!strcmp(sent_buf, "RCPT TO:<bob@gmail.com>\r\n"));

    reset_cap();
    feed_smtp_str(&sm, "250 2.1.5 OK\r\n");
    assert(!strcmp(sent_buf, "RCPT TO:<cc@gmail.com>\r\n"));

    reset_cap();
    feed_smtp_str(&sm, "250 2.1.5 OK\r\n");
    assert(!strcmp(sent_buf, "DATA\r\n"));

    reset_cap();
    feed_smtp_str(&sm, "354 Go ahead\r\n");
    /* Verify RFC 5321 dot-stuffing on ".Dot line" -> "..Dot line". */
    assert(strstr(sent_buf, "\r\n..Dot line\r\n") != NULL);
    assert(strstr(sent_buf, "\r\n.\r\n") != NULL);

    reset_cap();
    feed_smtp_str(&sm, "250 2.0.0 OK 1729550000\r\n");
    assert(!strcmp(sent_buf, "QUIT\r\n"));
    assert(nsmtp_evs > 0 && smtp_evs[nsmtp_evs - 1].type == SMTP_EV_SENT);

    smtp_reset(&sm);
}

/* ---- Local mock IMAP + SMTP server (--serve) ---------------------------- */

struct mock_mail {
    uint32_t uid;
    uint8_t flags;
    int mbox;
    char raw[4096];
};

static struct mock_mail store_msgs[16];
static int nstore_msgs;
static uint32_t next_mock_uid = 104;

static void init_mock_store(void)
{
    nstore_msgs = 3;
    store_msgs[0] = (struct mock_mail){
        .uid = 101,
        .flags = MAIL_F_SEEN,
        .mbox = MBOX_INBOX,
        .raw =
            "From: \"Casio Archive\" <archive@glaze-os.com>\r\n"
            "To: user@gmail.com\r\n"
            "Subject: Casio BE-300 Hardware Manual\r\n"
            "Date: Mon, 19 Oct 2026 09:15:00 -0700\r\n"
            "Message-ID: <m101@glaze-os.com>\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n"
            "The VR4131 runs at 166 MHz with 16 MiB SDRAM and a 240x320 RGB565 touch screen.\r\n",
    };
    store_msgs[1] = (struct mock_mail){
        .uid = 102,
        .flags = MAIL_F_FLAGGED,
        .mbox = MBOX_INBOX,
        .raw =
            "From: \"Ada Lovelace\" <ada@gmail.com>\r\n"
            "To: user@gmail.com\r\n"
            "Subject: Analytical Engine Notes\r\n"
            "Date: Tue, 20 Oct 2026 14:30:00 -0700\r\n"
            "Message-ID: <m102@gmail.com>\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n"
            "Please review the Bernoulli sequence table before tomorrow's demonstration.\r\n",
    };
    store_msgs[2] = (struct mock_mail){
        .uid = 103,
        .flags = 0,
        .mbox = MBOX_INBOX,
        .raw =
            "From: \"Glaze Team\" <hello@glaze-os.com>\r\n"
            "To: user@gmail.com\r\n"
            "Subject: Welcome to Gmail on Glaze\r\n"
            "Date: Wed, 21 Oct 2026 10:00:00 -0700\r\n"
            "Message-ID: <m103@glaze-os.com>\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n"
            "Welcome to Gmail for Glaze OS!\n\n"
            "You can browse your Inbox, Starred, Sent Mail, Drafts and Trash, "
            "search messages, and send replies right from your pocket BE-300.\r\n",
    };
}

static void fd_printf(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void fd_printf(int fd, const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)send(fd, buf, (size_t)n, 0);
}

static bool match_mbox(const struct mock_mail *m, int sel_mbox)
{
    if (sel_mbox == MBOX_STARRED)
        return (m->flags & MAIL_F_FLAGGED) && m->mbox != MBOX_TRASH;
    if (sel_mbox == MBOX_ALL)
        return m->mbox != MBOX_TRASH && m->mbox != MBOX_SPAM;
    return m->mbox == sel_mbox;
}

static void handle_mock_imap_line(int fd, int *sel_mbox, char *line)
{
    char *sp = strchr(line, ' ');
    if (!sp)
        return;
    *sp = 0;
    const char *tag = line;
    const char *cmd = sp + 1;
    fprintf(stderr, "imap: %s %s\n", tag, cmd);

    if (!strncasecmp(cmd, "CAPABILITY", 10)) {
        fd_printf(fd, "* CAPABILITY IMAP4rev1 X-GM-EXT-1\r\n%s OK CAPABILITY completed\r\n", tag);
    } else if (!strncasecmp(cmd, "LOGIN ", 6)) {
        fd_printf(fd, "%s OK LOGIN completed\r\n", tag);
    } else if (!strncasecmp(cmd, "LIST ", 5)) {
        fd_printf(fd,
                  "* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n"
                  "* LIST (\\Flagged \\HasNoChildren) \"/\" \"[Gmail]/Starred\"\r\n"
                  "* LIST (\\Sent \\HasNoChildren) \"/\" \"[Gmail]/Sent Mail\"\r\n"
                  "* LIST (\\Drafts \\HasNoChildren) \"/\" \"[Gmail]/Drafts\"\r\n"
                  "* LIST (\\All \\HasNoChildren) \"/\" \"[Gmail]/All Mail\"\r\n"
                  "* LIST (\\Junk \\HasNoChildren) \"/\" \"[Gmail]/Spam\"\r\n"
                  "* LIST (\\Trash \\HasNoChildren) \"/\" \"[Gmail]/Trash\"\r\n"
                  "%s OK LIST completed\r\n", tag);
    } else if (!strncasecmp(cmd, "STATUS ", 7)) {
        int total = 0, unseen = 0;
        for (int i = 0; i < nstore_msgs; i++) {
            if (store_msgs[i].mbox == MBOX_INBOX) {
                total++;
                if (!(store_msgs[i].flags & MAIL_F_SEEN))
                    unseen++;
            }
        }
        fd_printf(fd, "* STATUS \"INBOX\" (MESSAGES %d UNSEEN %d)\r\n%s OK STATUS completed\r\n",
                  total, unseen, tag);
    } else if (!strncasecmp(cmd, "SELECT ", 7)) {
        if (strstr(cmd, "Starred")) *sel_mbox = MBOX_STARRED;
        else if (strstr(cmd, "Sent")) *sel_mbox = MBOX_SENT;
        else if (strstr(cmd, "Drafts")) *sel_mbox = MBOX_DRAFTS;
        else if (strstr(cmd, "All Mail")) *sel_mbox = MBOX_ALL;
        else if (strstr(cmd, "Spam")) *sel_mbox = MBOX_SPAM;
        else if (strstr(cmd, "Trash")) *sel_mbox = MBOX_TRASH;
        else *sel_mbox = MBOX_INBOX;
        int count = 0;
        for (int i = 0; i < nstore_msgs; i++)
            if (match_mbox(&store_msgs[i], *sel_mbox))
                count++;
        fd_printf(fd, "* %d EXISTS\r\n%s OK [READ-WRITE] SELECT completed\r\n", count, tag);
    } else if (!strncasecmp(cmd, "FETCH ", 6) || !strncasecmp(cmd, "UID FETCH ", 10)) {
        bool is_uid = !strncasecmp(cmd, "UID ", 4);
        bool full = strstr(cmd, "BODY[]") != NULL;
        uint32_t want_uid = is_uid ? (uint32_t)strtoul(cmd + 10, NULL, 10) : 0;
        int seq = 0;
        for (int i = 0; i < nstore_msgs; i++) {
            struct mock_mail *m = &store_msgs[i];
            if (!match_mbox(m, *sel_mbox))
                continue;
            seq++;
            if (is_uid && full && m->uid != want_uid)
                continue;
            if (full)
                m->flags |= MAIL_F_SEEN;
            char fl[64] = "";
            if (m->flags & MAIL_F_SEEN) strlcat(fl, "\\Seen", sizeof(fl));
            if (m->flags & MAIL_F_FLAGGED) strlcat(fl, fl[0] ? " \\Flagged" : "\\Flagged", sizeof(fl));
            const char *payload = m->raw;
            size_t plen = strlen(m->raw);
            if (!full) {
                const char *sep = strstr(m->raw, "\r\n\r\n");
                if (sep)
                    plen = (size_t)((sep + 4) - m->raw);
            }
            fd_printf(fd, "* %d FETCH (UID %u FLAGS (%s) INTERNALDATE \"21-Oct-2026 10:00:00 -0700\" %s {%zu}\r\n",
                      seq, m->uid, fl,
                      full ? "BODY[]<0>" : "BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)]",
                      plen);
            (void)send(fd, payload, plen, 0);
            fd_printf(fd, ")\r\n");
        }
        fd_printf(fd, "%s OK FETCH completed\r\n", tag);
    } else if (!strncasecmp(cmd, "UID SEARCH ", 11)) {
        const char *q = strrchr(cmd, '"');
        char term[64] = "";
        if (q && q > cmd) {
            const char *q0 = q - 1;
            while (q0 > cmd && *q0 != '"')
                q0--;
            if (*q0 == '"') {
                size_t l = (size_t)(q - (q0 + 1));
                if (l >= sizeof(term)) l = sizeof(term) - 1;
                memcpy(term, q0 + 1, l);
                term[l] = 0;
            }
        }
        fd_printf(fd, "* SEARCH");
        for (int i = 0; i < nstore_msgs; i++) {
            if (!term[0] || strcasestr(store_msgs[i].raw, term))
                fd_printf(fd, " %u", store_msgs[i].uid);
        }
        fd_printf(fd, "\r\n%s OK SEARCH completed\r\n", tag);
    } else if (!strncasecmp(cmd, "UID STORE ", 10)) {
        uint32_t uid = (uint32_t)strtoul(cmd + 10, NULL, 10);
        bool add = strchr(cmd, '+') != NULL;
        for (int i = 0; i < nstore_msgs; i++) {
            if (store_msgs[i].uid == uid) {
                if (strstr(cmd, "\\Flagged")) {
                    if (add) store_msgs[i].flags |= MAIL_F_FLAGGED;
                    else store_msgs[i].flags &= (uint8_t)~MAIL_F_FLAGGED;
                }
                if (strstr(cmd, "\\Seen")) {
                    if (add) store_msgs[i].flags |= MAIL_F_SEEN;
                    else store_msgs[i].flags &= (uint8_t)~MAIL_F_SEEN;
                }
                if (strstr(cmd, "\\Deleted") && add) {
                    store_msgs[i].mbox = MBOX_ALL;
                }
            }
        }
        fd_printf(fd, "%s OK STORE completed\r\n", tag);
    } else if (!strncasecmp(cmd, "UID COPY ", 9)) {
        uint32_t uid = (uint32_t)strtoul(cmd + 9, NULL, 10);
        for (int i = 0; i < nstore_msgs; i++)
            if (store_msgs[i].uid == uid && strstr(cmd, "Trash"))
                store_msgs[i].mbox = MBOX_TRASH;
        fd_printf(fd, "%s OK COPY completed\r\n", tag);
    } else if (!strncasecmp(cmd, "EXPUNGE", 7) || !strncasecmp(cmd, "NOOP", 4)) {
        fd_printf(fd, "%s OK Completed\r\n", tag);
    } else if (!strncasecmp(cmd, "LOGOUT", 6)) {
        fd_printf(fd, "* BYE LOGOUT\r\n%s OK LOGOUT completed\r\n", tag);
    } else {
        fd_printf(fd, "%s OK Completed\r\n", tag);
    }
}

static int listen_tcp(uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    assert(s >= 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        exit(1);
    }
    assert(listen(s, 4) == 0);
    return s;
}

static int serve_mode(uint16_t imap_port, uint16_t smtp_port)
{
    init_mock_store();
    int ls_imap = listen_tcp(imap_port);
    int ls_smtp = listen_tcp(smtp_port);
    int c_imap = -1, c_smtp = -1;
    int sel_mbox = MBOX_INBOX;
    char iline[1024], sline[1024], sdata[4096];
    size_t ilen = 0, slen = 0, dlen = 0;
    bool in_data = false;

    int secs = getenv("GMAIL_SECS") ? atoi(getenv("GMAIL_SECS")) : 90;
    for (int tick = 0; tick < secs * 20; tick++) {
        struct pollfd pfd[4] = {
            { ls_imap, POLLIN, 0 },
            { ls_smtp, POLLIN, 0 },
            { c_imap, POLLIN, 0 },
            { c_smtp, POLLIN, 0 },
        };
        if (poll(pfd, 4, 50) <= 0)
            continue;
        if (pfd[0].revents & POLLIN) {
            if (c_imap >= 0) close(c_imap);
            c_imap = accept(ls_imap, NULL, NULL);
            ilen = 0;
            sel_mbox = MBOX_INBOX;
            fd_printf(c_imap, "* OK Gimap mock server ready\r\n");
        }
        if (pfd[1].revents & POLLIN) {
            if (c_smtp >= 0) close(c_smtp);
            c_smtp = accept(ls_smtp, NULL, NULL);
            slen = dlen = 0;
            in_data = false;
            fd_printf(c_smtp, "220 smtp.gmail.com ESMTP mock ready\r\n");
        }
        if (c_imap >= 0 && (pfd[2].revents & (POLLIN | POLLHUP))) {
            char buf[512];
            ssize_t n = recv(c_imap, buf, sizeof(buf), 0);
            if (n <= 0) {
                close(c_imap);
                c_imap = -1;
            } else {
                for (ssize_t i = 0; i < n; i++) {
                    if (buf[i] == '\r') continue;
                    if (buf[i] == '\n') {
                        iline[ilen] = 0;
                        ilen = 0;
                        handle_mock_imap_line(c_imap, &sel_mbox, iline);
                    } else if (ilen + 1 < sizeof(iline)) {
                        iline[ilen++] = buf[i];
                    }
                }
            }
        }
        if (c_smtp >= 0 && (pfd[3].revents & (POLLIN | POLLHUP))) {
            char buf[512];
            ssize_t n = recv(c_smtp, buf, sizeof(buf), 0);
            if (n <= 0) {
                close(c_smtp);
                c_smtp = -1;
            } else {
                for (ssize_t i = 0; i < n; i++) {
                    if (buf[i] == '\r') continue;
                    if (buf[i] == '\n') {
                        sline[slen] = 0;
                        slen = 0;
                        if (!in_data)
                            fprintf(stderr, "smtp: %s\n", sline);
                        if (in_data) {
                            if (!strcmp(sline, ".")) {
                                in_data = false;
                                if (nstore_msgs < 16) {
                                    struct mock_mail *m = &store_msgs[nstore_msgs++];
                                    m->uid = next_mock_uid++;
                                    m->flags = MAIL_F_SEEN;
                                    m->mbox = MBOX_SENT;
                                    strlcpy(m->raw, sdata, sizeof(m->raw));
                                }
                                fprintf(stderr, "smtp: queued %zu bytes:\n%s", dlen, sdata);
                                fd_printf(c_smtp, "250 2.0.0 OK Queued\r\n");
                            } else {
                                const char *l = (sline[0] == '.' && sline[1] == '.') ? sline + 1 : sline;
                                size_t ll = strlen(l);
                                if (dlen + ll + 3 < sizeof(sdata)) {
                                    memcpy(sdata + dlen, l, ll);
                                    dlen += ll;
                                    sdata[dlen++] = '\r';
                                    sdata[dlen++] = '\n';
                                    sdata[dlen] = 0;
                                }
                            }
                        } else if (!strncasecmp(sline, "EHLO", 4)) {
                            fd_printf(c_smtp, "250-smtp.gmail.com\r\n250 AUTH PLAIN LOGIN\r\n");
                        } else if (!strncasecmp(sline, "AUTH ", 5)) {
                            fd_printf(c_smtp, "235 2.7.0 Accepted\r\n");
                        } else if (!strncasecmp(sline, "MAIL FROM:", 10) || !strncasecmp(sline, "RCPT TO:", 8)) {
                            fd_printf(c_smtp, "250 OK\r\n");
                        } else if (!strncasecmp(sline, "DATA", 4)) {
                            in_data = true;
                            dlen = 0;
                            sdata[0] = 0;
                            fd_printf(c_smtp, "354 End data with <CR><LF>.<CR><LF>\r\n");
                        } else if (!strncasecmp(sline, "QUIT", 4)) {
                            fd_printf(c_smtp, "221 Bye\r\n");
                        }
                    } else if (slen + 1 < sizeof(sline)) {
                        sline[slen++] = buf[i];
                    }
                }
            }
        }
    }
    if (c_imap >= 0) close(c_imap);
    if (c_smtp >= 0) close(c_smtp);
    close(ls_imap);
    close(ls_smtp);
    return 0;
}

/* ---- Live client mode (--live) ------------------------------------------ */

struct live_conn {
    int fd;
    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    struct imap *im;
};

static int live_io_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    struct live_conn *c = ctx;
    (void)ssl;
    ssize_t r = recv(c->fd, buf, (size_t)sz, 0);
    if (r > 0)
        return (int)r;
    if (r == 0)
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_GENERAL;
}

static int live_io_send(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    struct live_conn *c = ctx;
    (void)ssl;
    ssize_t r = send(c->fd, buf, (size_t)sz, 0);
    return r >= 0 ? (int)r : WOLFSSL_CBIO_ERR_GENERAL;
}

static uint32_t der_cert_len(const uint8_t *d, uint32_t avail)
{
    if (avail < 2 || d[0] != 0x30)
        return 0;
    if (d[1] < 0x80)
        return 2u + d[1] <= avail ? 2u + d[1] : 0;
    int nb = d[1] & 0x7F;
    if (nb < 1 || nb > 3 || avail < 2u + (uint32_t)nb)
        return 0;
    uint32_t len = 0;
    for (int i = 0; i < nb; i++)
        len = (len << 8) | d[2 + i];
    len += 2u + (uint32_t)nb;
    return len <= avail ? len : 0;
}

static bool live_connect(struct live_conn *c, const char *host, uint16_t port, bool use_tls)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    if (getaddrinfo(host, NULL, &hints, &ai) != 0)
        return false;
    struct sockaddr_in sa;
    memcpy(&sa, ai->ai_addr, sizeof(sa));
    freeaddrinfo(ai);
    sa.sin_port = htons(port);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0)
        return false;
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(c->fd);
        c->fd = -1;
        return false;
    }
    if (!use_tls)
        return true;

    wolfSSL_Init();
    c->ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!c->ctx)
        return false;
    wolfSSL_CTX_set_verify(c->ctx, WOLFSSL_VERIFY_NONE, NULL);
    FILE *rf = fopen("../assets/ssl/roots.der", "rb");
    if (!rf)
        rf = fopen("assets/ssl/roots.der", "rb");
    if (rf) {
        fseek(rf, 0, SEEK_END);
        long sz = ftell(rf);
        rewind(rf);
        uint8_t *der = malloc((size_t)sz);
        if (der && fread(der, 1, (size_t)sz, rf) == (size_t)sz) {
            wolfSSL_CTX_set_verify(c->ctx, WOLFSSL_VERIFY_PEER, NULL);
            for (uint32_t off = 0; off < (uint32_t)sz;) {
                uint32_t n = der_cert_len(der + off, (uint32_t)sz - off);
                if (!n)
                    break;
                wolfSSL_CTX_load_verify_buffer(c->ctx, der + off, n, WOLFSSL_FILETYPE_ASN1);
                off += n;
            }
        }
        free(der);
        fclose(rf);
    }
    c->ssl = wolfSSL_new(c->ctx);
    if (!c->ssl)
        return false;
    wolfSSL_UseSNI(c->ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
    wolfSSL_SSLSetIORecv(c->ssl, live_io_recv);
    wolfSSL_SSLSetIOSend(c->ssl, live_io_send);
    wolfSSL_SetIOReadCtx(c->ssl, c);
    wolfSSL_SetIOWriteCtx(c->ssl, c);
    return wolfSSL_connect(c->ssl) == WOLFSSL_SUCCESS;
}

static void live_close(struct live_conn *c)
{
    if (c->ssl)
        wolfSSL_free(c->ssl);
    if (c->ctx)
        wolfSSL_CTX_free(c->ctx);
    if (c->fd >= 0)
        close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static int live_send_cb(void *ctx, const void *buf, size_t len)
{
    struct live_conn *c = ctx;
    if (c->ssl)
        return wolfSSL_write(c->ssl, buf, (int)len) == (int)len ? 0 : -1;
    return send(c->fd, buf, len, 0) == (ssize_t)len ? 0 : -1;
}

static bool live_done;

static void live_imap_ev(void *ctx, const struct imap_event *ev)
{
    struct live_conn *c = ctx;
    struct imap *im = c->im;
    switch (ev->type) {
    case IMAP_EV_PROGRESS:
        printf("[imap] %s\n", ev->text);
        break;
    case IMAP_EV_READY:
        printf("[imap] READY: %d messages in Inbox (%u unseen)\n", im->nmsgs, im->inbox_unseen);
        for (int i = 0; i < im->nmsgs; i++)
            printf("  #%d UID %u [%s] %s — %s (%s)\n", i + 1, im->msgs[i].uid,
                   (im->msgs[i].flags & MAIL_F_SEEN) ? "read" : "NEW",
                   im->msgs[i].from_name, im->msgs[i].subject, im->msgs[i].date);
        if (im->nmsgs > 0)
            imap_fetch_message(im, im->msgs[0].uid);
        else
            live_done = true;
        break;
    case IMAP_EV_MSG_DONE:
        printf("[imap] Latest message UID %u:\nSubject: %s\n---\n%s\n---\n",
               im->cur_msg.hdr.uid, im->cur_msg.hdr.subject, im->cur_msg.body);
        live_done = true;
        break;
    case IMAP_EV_ERROR:
        fprintf(stderr, "[imap] ERROR: %s\n", ev->text);
        live_done = true;
        break;
    default:
        break;
    }
}

static void live_smtp_ev(void *ctx, const struct smtp_event *ev)
{
    (void)ctx;
    if (ev->type == SMTP_EV_PROGRESS)
        printf("[smtp] %s\n", ev->text);
    else if (ev->type == SMTP_EV_SENT) {
        printf("[smtp] SENT OK\n");
        live_done = true;
    } else if (ev->type == SMTP_EV_ERROR) {
        fprintf(stderr, "[smtp] ERROR: %s\n", ev->text);
        live_done = true;
    }
}

static int live_mode(int argc, char **argv)
{
    const char *ihost = argv[2];
    uint16_t iport = (uint16_t)atoi(argv[3]);
    const char *shost = argv[4];
    uint16_t sport = (uint16_t)atoi(argv[5]);
    const char *user = argv[6];
    const char *pass = argv[7];

    if (argc > 8) {
        const char *to = argv[8];
        const char *subj = argc > 9 ? argv[9] : "Hello from Glaze";
        const char *body = argc > 10 ? argv[10] : "Sent by gmail-test --live.";
        struct live_conn sc;
        if (!live_connect(&sc, shost, sport, sport == 465)) {
            fprintf(stderr, "smtp connect failed\n");
            return 1;
        }
        char raw[2048];
        size_t rlen = mime_build_message(raw, sizeof(raw), user, user, to, NULL, subj,
                                         NULL, NULL, NULL, NULL, body);
        struct smtp sm;
        smtp_init(&sm, &(struct smtp_ops){ &sc, live_send_cb, live_smtp_ev });
        smtp_start(&sm, user, pass, user, to, NULL, raw, rlen);
        live_done = false;
        while (!live_done) {
            uint8_t buf[1024];
            int n = sc.ssl ? wolfSSL_read(sc.ssl, buf, sizeof(buf)) : (int)recv(sc.fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            smtp_feed(&sm, buf, (size_t)n);
        }
        smtp_reset(&sm);
        live_close(&sc);
    }

    struct live_conn ic;
    if (!live_connect(&ic, ihost, iport, iport == 993)) {
        fprintf(stderr, "imap connect failed\n");
        return 1;
    }
    struct imap im;
    ic.im = &im;
    imap_init(&im, &(struct imap_ops){ &ic, live_send_cb, live_imap_ev });
    imap_login(&im, user, pass);
    live_done = false;
    while (!live_done) {
        uint8_t buf[2048];
        int n = ic.ssl ? wolfSSL_read(ic.ssl, buf, sizeof(buf)) : (int)recv(ic.fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        /* Temporarily switch ctx for event delivery during feed. */
        imap_feed(&im, buf, (size_t)n);
    }
    imap_reset(&im);
    live_close(&ic);
    return 0;
}

/* ---- Native UI, Persistence, and Screenshot Harness --------------------- */

#define main gmail_app_main
#define acct gmail_acct
#include "../packages/gmail/main.c"
#undef main
#include "../host/hostres.h"

struct ui_app ui;
static uint16_t ui_pixels[UI_W * UI_H];
static uint32_t fake_clock_ms = 1000;
static int last_badge = -1;
static int last_sound = -1;

/* In-memory bundle filesystem for account, cache, and draft persistence. */
static struct {
    char path[FS_PATH_MAX];
    uint8_t data[65536];
    uint32_t len;
    bool used;
} fake_fs[16];

static int mock_ch_state[CONN_CH_COUNT];
static char mock_rx[CONN_CH_COUNT][32768];
static size_t mock_rx_len[CONN_CH_COUNT], mock_rx_pos[CONN_CH_COUNT];

uint32_t gz_time_ms(void) { return fake_clock_ms; }
void gz_sleep_ms(uint32_t ms) { fake_clock_ms += ms; }
uint32_t ui_now_secs(void) { return 1792600000u + fake_clock_ms / 1000u; }
int gz_rtc_get(struct gz_datetime *dt)
{
    *dt = (struct gz_datetime){ 2026, 10, 21, 16, 5, 9, 3 };
    return 0;
}
int pref_get(const char *key, void *buf, uint32_t len) { (void)key; (void)buf; (void)len; return GZ_ENOENT; }
int pref_put(const char *key,const void *buf,uint32_t len){(void)key;(void)buf;(void)len;return 0;}
int ui_set_badge(int count) { last_badge = count; return 0; }
static bool fake_background;
static char last_note[160];
bool ui_in_background(void) { return fake_background; }
void ui_set_background_running(bool on) { (void)on; }
int ui_notify_post(uint32_t id, uint32_t when, const char *title, const char *body, int badge)
{
    (void)id;
    (void)when;
    (void)badge;
    snprintf(last_note, sizeof(last_note), "%s: %s", title, body);
    return 0;
}
void ui_init(int style) { (void)style; }
int ui_next_event(struct gz_input_event *e, int timeout) { (void)e; (void)timeout; return 0; }
void ui_invalidate(gfx_rect r) { ui.dirty = gfx_rect_union(ui.dirty, r); }
void ui_invalidate_all(void) { ui_invalidate(gfx_r(0, 0, UI_W, UI_H)); }
void ui_present(void) { ui.dirty = gfx_r(0, 0, 0, 0); }
void uisnd_event(const struct gz_input_event *e) { (void)e; }
void uisnd_sounded(void) {}
int audio_sound(int snd) { (void)snd; return 0; }
int ui_ease_out(uint32_t t, uint32_t dur) { return t >= dur ? 1024 : (int)(t * 1024 / dur); }
const char *res_bundle(void) { return "/Applications/gmail.app"; }

int fs_mkdirs(const char *path) { (void)path; return 0; }

int fs_read_file(const char *path, void *buf, uint32_t max)
{
    for (int i = 0; i < 16; i++) {
        if (fake_fs[i].used && !strcmp(fake_fs[i].path, path)) {
            uint32_t n = fake_fs[i].len < max ? fake_fs[i].len : max;
            memcpy(buf, fake_fs[i].data, n);
            return (int)n;
        }
    }
    return GZ_ENOENT;
}

int fs_write_file(const char *path, const void *buf, uint32_t len)
{
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (fake_fs[i].used && !strcmp(fake_fs[i].path, path)) {
            slot = i;
            break;
        }
        if (!fake_fs[i].used && slot < 0)
            slot = i;
    }
    assert(slot >= 0 && len <= sizeof(fake_fs[0].data));
    fake_fs[slot].used = true;
    strlcpy(fake_fs[slot].path, path, sizeof(fake_fs[slot].path));
    memcpy(fake_fs[slot].data, buf, len);
    fake_fs[slot].len = len;
    return (int)len;
}

void conn_start(int ch, const char *host, uint16_t port, bool use_tls)
{
    (void)host;
    (void)port;
    (void)use_tls;
    mock_ch_state[ch] = CONN_UP;
}

int conn_state(int ch) { return mock_ch_state[ch]; }
const char *conn_error(int ch) { (void)ch; return "mock error"; }
static char mock_tx_smtp[MAIL_RAW_OUT_MAX + 1024];
static size_t mock_tx_smtp_len;

int conn_send(int ch, const void *buf, size_t len)
{
    if (ch == CONN_CH_SMTP && mock_tx_smtp_len + len < sizeof(mock_tx_smtp)) {
        memcpy(mock_tx_smtp + mock_tx_smtp_len, buf, len);
        mock_tx_smtp_len += len;
        mock_tx_smtp[mock_tx_smtp_len] = 0;
    }
    return (int)len;
}

int conn_recv(int ch, void *buf, size_t cap)
{
    size_t rem = mock_rx_len[ch] - mock_rx_pos[ch];
    if (rem == 0)
        return 0;
    size_t n = rem < cap ? rem : cap;
    memcpy(buf, mock_rx[ch] + mock_rx_pos[ch], n);
    mock_rx_pos[ch] += n;
    return (int)n;
}

void conn_close(int ch)
{
    mock_ch_state[ch] = CONN_NONE;
    mock_rx_len[ch] = mock_rx_pos[ch] = 0;
}

void sound_play(int which) { last_sound = which; }
bool sound_pump(void) { return false; }

static void queue_mock_rx(int ch, const char *s)
{
    size_t n = strlen(s);
    assert(mock_rx_len[ch] + n < sizeof(mock_rx[0]));
    memcpy(mock_rx[ch] + mock_rx_len[ch], s, n);
    mock_rx_len[ch] += n;
}

static void ui_shot(const char *name)
{
    char path[128];
    redraw();
    snprintf(path, sizeof(path), "../out/host/gmail-%s.bmp", name);
    host_write_bmp(path, &ui.surf);
}

static void sim_tap(int x, int y)
{
    struct gz_input_event down = { .type = GZ_EV_TOUCH_DOWN, .x = (int16_t)x, .y = (int16_t)y };
    struct gz_input_event up   = { .type = GZ_EV_TOUCH_UP,   .x = (int16_t)x, .y = (int16_t)y };
    touch_event(&down);
    fake_clock_ms += 20;
    touch_event(&up);
    if (dirty)
        redraw();
}

static void sim_type(const char *s)
{
    for (; *s; s++) {
        field_key((unsigned char)*s);
        fake_clock_ms += 10;
    }
    if (dirty)
        redraw();
}

/* One pass of main()'s loop per call until the mock channels are drained. */
static void pump(void)
{
    for (int i = 0; i < 16; i++) {
        service_network();
        if (mock_rx_pos[CONN_CH_IMAP] == mock_rx_len[CONN_CH_IMAP] &&
            mock_rx_pos[CONN_CH_SMTP] == mock_rx_len[CONN_CH_SMTP])
            break;
    }
    if (dirty)
        redraw();
}

static void test_ui(void)
{
    gfx_surface_init(&ui.surf, ui_pixels, UI_W, UI_H, UI_W);
    load_account();
    load_cache();
    load_draft();
    update_badge();
    ui_list_init(&mlist, gfx_r(0, UI_NAV_H, UI_W, UI_H - UI_NAV_H - TOOLBAR_H), MSG_ROW_H);
    imap_init(&im, &(struct imap_ops){ NULL, imap_send_hook, on_imap_event });
    smtp_init(&sm, &(struct smtp_ops){ NULL, smtp_send_hook, on_smtp_event });

    view = V_SIGNON;
    ui_shot("signon");
    assert(view == V_SIGNON);

    /* Open Server settings and return. */
    sim_tap(210, 15);
    assert(view == V_SERVER);
    ui_shot("server");
    sim_tap(30, 15);
    assert(view == V_SIGNON);

    /* Focus Email, type credentials, and sign in. */
    sim_tap(150, 86);
    assert(focus == &f_email);
    sim_type("alex@gmail.com\n");
    assert(focus == &f_pass);
    sim_type("abcd-efgh-ijkl-mnop\n");
    assert(phase == P_CONNECTING);

    const char *hdr101 =
        "From: \"Google Team\" <mail-noreply@google.com>\r\n"
        "To: alex@gmail.com\r\n"
        "Subject: Welcome to Gmail on Glaze OS\r\n"
        "Date: Tue, 20 Oct 2026 09:15:00 -0700\r\n"
        "Message-ID: <101@gmail.glaze>\r\n\r\n";
    const char *hdr102 =
        "From: \"Sarah Chen\" <sarah@example.com>\r\n"
        "To: alex@gmail.com\r\n"
        "Cc: team@example.com\r\n"
        "Subject: Glaze OS 1.0 Release Notes & Schedule\r\n"
        "Date: Wed, 21 Oct 2026 14:30:00 -0700\r\n"
        "Message-ID: <102@gmail.glaze>\r\n\r\n";
    char login_stream[4096];
    snprintf(login_stream, sizeof(login_stream),
             "* OK Gimap ready\r\n"
             "* CAPABILITY IMAP4rev1 X-GM-EXT-1\r\n"
             "A001 OK CAPABILITY completed\r\n"
             "A002 OK LOGIN completed\r\n"
             "* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n"
             "* LIST (\\Flagged) \"/\" \"[Gmail]/Starred\"\r\n"
             "* LIST (\\Sent) \"/\" \"[Gmail]/Sent Mail\"\r\n"
             "* LIST (\\Drafts) \"/\" \"[Gmail]/Drafts\"\r\n"
             "* LIST (\\All) \"/\" \"[Gmail]/All Mail\"\r\n"
             "* LIST (\\Junk) \"/\" \"[Gmail]/Spam\"\r\n"
             "* LIST (\\Trash) \"/\" \"[Gmail]/Trash\"\r\n"
             "A003 OK LIST completed\r\n"
             "* STATUS \"INBOX\" (MESSAGES 2 UNSEEN 1)\r\n"
             "A004 OK STATUS completed\r\n"
             "* 2 EXISTS\r\n"
             "A005 OK [READ-WRITE] SELECT completed\r\n"
             "* 1 FETCH (UID 101 FLAGS (\\Seen) BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n"
             "%s)\r\n"
             "* 2 FETCH (UID 102 FLAGS (\\Flagged) BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n"
             "%s)\r\n"
             "A006 OK FETCH completed\r\n",
             strlen(hdr101), hdr101, strlen(hdr102), hdr102);
    queue_mock_rx(CONN_CH_IMAP, login_stream);
    pump();
    assert(phase == P_ONLINE);
    assert(view == V_LIST);
    assert(im.nmsgs == 2);
    assert(last_badge == 1);
    ui_shot("inbox");

    /* Navigate back to Mailboxes view and capture screenshot. */
    sim_tap(40, 15);
    assert(view == V_MAILBOXES);
    ui_shot("mailboxes");

    /* Tap Inbox row in Mailboxes. */
    sim_tap(120, 75);
    assert(view == V_LIST);
    queue_mock_rx(CONN_CH_IMAP,
                  "* 2 EXISTS\r\n"
                  "A007 OK SELECT completed\r\n");
    pump();
    char fetch_again[2048];
    snprintf(fetch_again, sizeof(fetch_again),
             "* 1 FETCH (UID 101 FLAGS (\\Seen) BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n"
             "%s)\r\n"
             "* 2 FETCH (UID 102 FLAGS (\\Flagged) BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n"
             "%s)\r\n"
             "A008 OK FETCH completed\r\n",
             strlen(hdr101), hdr101, strlen(hdr102), hdr102);
    queue_mock_rx(CONN_CH_IMAP, fetch_again);
    pump();

    /* Tap first message (UID 102, newest first) to open V_MESSAGE. */
    sim_tap(120, 55);
    assert(view == V_MESSAGE);
    const char *full102 =
        "From: \"Sarah Chen\" <sarah@example.com>\r\n"
        "To: alex@gmail.com\r\n"
        "Cc: team@example.com\r\n"
        "Subject: Glaze OS 1.0 Release Notes & Schedule\r\n"
        "Date: Wed, 21 Oct 2026 14:30:00 -0700\r\n"
        "Message-ID: <102@gmail.glaze>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"b102\"\r\n\r\n"
        "--b102\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n\r\n"
        "Hi Alex,\r\n\r\n"
        "Here is the draft checklist for the Glaze OS 1.0 release:\r\n"
        "  1. Kernel & ROMFS paging verified\r\n"
        "  2. App Store catalog & Gmail client ready\r\n\r\n"
        "> Can we target Friday morning for the announcement?\r\n\r\n"
        "Yes! Let me know if the notes look good.\r\n\r\n"
        "Best,\r\nSarah\r\n"
        "--b102\r\n"
        "Content-Type: application/pdf; name=\"release-checklist.pdf\"\r\n"
        "Content-Disposition: attachment; filename=\"release-checklist.pdf\"\r\n"
        "Content-Transfer-Encoding: base64\r\n\r\n"
        "JVBERi0xLjQKMSAwIG9iago8PCAvVHlwZSAvQ2F0YWxvZyA+PgplbmRvYmoK\r\n"
        "--b102--\r\n";
    char body_resp[4096];
    snprintf(body_resp, sizeof(body_resp),
             "* 2 FETCH (UID 102 FLAGS (\\Seen \\Flagged) BODY[] {%zu}\r\n%s)\r\n"
             "A009 OK UID FETCH completed\r\n",
             strlen(full102), full102);
    queue_mock_rx(CONN_CH_IMAP, body_resp);
    pump();
    assert(!msg_loading);
    assert(im.cur_msg.natts == 1);
    assert(last_badge == 0);
    ui_shot("message");

    /* Tap Reply in toolbar -> Respond alert -> tap Reply -> V_COMPOSE. */
    sim_tap(118, 283);
    assert(alert_kind == ALERT_REPLY_KIND);
    sim_tap(alert_r1.x + alert_r1.w / 2, alert_r1.y + alert_r1.h / 2);
    assert(view == V_COMPOSE);
    assert(!strcmp(draft.to, "sarah@example.com"));
    assert(!strcmp(draft.subject, "Re: Glaze OS 1.0 Release Notes & Schedule"));
    sim_type("Looks great, let's ship it Friday at 9am!");
    ui_shot("compose");

    /* Tap Done to hide keyboard and show full compose preview, then Send. */
    sim_tap(28, 15);
    assert(focus == NULL);
    ui_shot("compose-ready");

    sim_tap(212, 15);
    assert(smtp_busy);
    queue_mock_rx(CONN_CH_SMTP,
                  "220 smtp.gmail.com ESMTP\r\n"
                  "250-smtp.gmail.com\r\n"
                  "250 AUTH PLAIN LOGIN\r\n"
                  "235 2.7.0 Accepted\r\n"
                  "250 2.1.0 OK\r\n"
                  "250 2.1.5 OK\r\n"
                  "354 Go ahead\r\n"
                  "250 2.0.0 OK queued\r\n"
                  "221 2.0.0 closing\r\n");
    pump();
    assert(!smtp_busy);
    assert(last_sound == SND_MAIL_SENT);
    assert(strstr(mock_tx_smtp, "\r\nDate: Wed, 21 Oct 2026 16:05:09 -0700\r\n"));
    assert(strstr(mock_tx_smtp, "\r\nIn-Reply-To: <102@gmail.glaze>\r\n"));

    /* After Home: new mail found by the NOOP poll refreshes the inbox and
     * posts a note instead of playing the sound. */
    while (im.active_cmd != ICMD_NONE) {
        char done[64];
        snprintf(done, sizeof(done), "%s OK done\r\n", im.active_tag);
        queue_mock_rx(CONN_CH_IMAP, done);
        pump();
    }
    fake_background = true;
    last_sound = -1;
    fake_clock_ms += 61000;
    imap_tick(&im, gz_time_ms());
    fake_clock_ms += 61000;
    imap_tick(&im, gz_time_ms());
    assert(im.active_cmd == ICMD_NOOP);
    const char *hdr103 =
        "From: \"Ada Lovelace\" <ada@gmail.com>\r\n"
        "Subject: Analytical Engine Notes\r\n\r\n";
    char noop[1024];
    snprintf(noop, sizeof(noop), "* 3 EXISTS\r\n%s OK NOOP completed\r\n", im.active_tag);
    queue_mock_rx(CONN_CH_IMAP, noop);
    pump();
    assert(last_sound == -1 && im.active_cmd == ICMD_SELECT);
    snprintf(noop, sizeof(noop), "* 3 EXISTS\r\n%s OK SELECT completed\r\n", im.active_tag);
    queue_mock_rx(CONN_CH_IMAP, noop);
    pump();
    char list3[2048];
    snprintf(list3, sizeof(list3),
             "* 3 FETCH (UID 103 FLAGS () BODY[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID)] {%zu}\r\n%s)\r\n"
             "%s OK FETCH completed\r\n",
             strlen(hdr103), hdr103, im.active_tag);
    queue_mock_rx(CONN_CH_IMAP, list3);
    pump();
    assert(!strcmp(last_note, "Ada Lovelace: Analytical Engine Notes"));
    assert(view == V_MESSAGE);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--serve")) {
        return serve_mode((uint16_t)atoi(argv[2]), (uint16_t)atoi(argv[3]));
    }
    if (argc >= 8 && !strcmp(argv[1], "--live")) {
        return live_mode(argc, argv);
    }
    test_mime();
    test_imap();
    test_smtp();
    test_ui();
    printf("gmail: ALL PASS\n");
    return 0;
}
