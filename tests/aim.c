/*
 * The AIM package's protocol engine (packages/aim/oscar.c), natively: MD5
 * vectors, HTML to text, and a scripted OSCAR session fed byte by byte.
 *
 *   aim-test                                   unit tests
 *   aim-test --live host port sn password [buddy message]
 *                                              sign on to a real server,
 *                                              optionally IM a buddy, and
 *                                              print events for 20 s (or
 *                                              $AIM_SECS), echoing IMs
 */
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "../packages/aim/oscar.h"

/* ---- capture ------------------------------------------------------------ */

static uint8_t sent[1 << 16];
static size_t sent_len, sent_off;
static struct oscar o;

#define MAX_EVENTS 64
static struct {
    enum oscar_event_type type;
    char name[64], text[1100], text2[1100];
    int flag;
} events[MAX_EVENTS];
static int nevents;

static int cap_send(void *ctx, const void *buf, size_t len)
{
    assert(sent_len + len <= sizeof(sent));
    memcpy(sent + sent_len, buf, len);
    sent_len += len;
    return 0;
}

static void cap_event(void *ctx, const struct oscar_event *ev)
{
    assert(nevents < MAX_EVENTS);
    events[nevents].type = ev->type;
    snprintf(events[nevents].name, sizeof(events[0].name), "%s", ev->name ? ev->name : "");
    snprintf(events[nevents].text, sizeof(events[0].text), "%s", ev->text ? ev->text : "");
    snprintf(events[nevents].text2, sizeof(events[0].text2), "%s", ev->text2 ? ev->text2 : "");
    events[nevents].flag = ev->flag;
    nevents++;
}

static int find_event(enum oscar_event_type type)
{
    for (int i = 0; i < nevents; i++)
        if (events[i].type == type)
            return i;
    return -1;
}

/* Next FLAP the engine sent: its channel, and body. */
static int next_flap(const uint8_t **body, size_t *len)
{
    assert(sent_off + 6 <= sent_len);
    const uint8_t *p = sent + sent_off;
    assert(p[0] == '*');
    *len = (size_t)p[4] << 8 | p[5];
    *body = p + 6;
    sent_off += 6 + *len;
    assert(sent_off <= sent_len);
    return p[1];
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* Next SNAC sent, which must be fam/sub; returns its body. */
static const uint8_t *expect_snac(unsigned fam, unsigned sub, size_t *len)
{
    const uint8_t *b;
    size_t n;
    int ch = next_flap(&b, &n);
    if (ch != 2 || be16(b) != fam || be16(b + 2) != sub) {
        fprintf(stderr, "expected SNAC %02x,%02x, got channel %d %02x,%02x\n", fam, sub, ch, n >= 4 ? be16(b) : 0,
                n >= 4 ? be16(b + 2) : 0);
        abort();
    }
    if (len)
        *len = n - 10;
    return b + 10;
}

static void expect_nothing_sent(void)
{
    assert(sent_off == sent_len);
}

static const uint8_t *tlv(const uint8_t *p, size_t len, unsigned type, size_t *vlen)
{
    for (size_t off = 0; off + 4 <= len;) {
        unsigned t = be16(p + off), n = be16(p + off + 2);
        if (t == type) {
            *vlen = n;
            return p + off + 4;
        }
        off += 4 + n;
    }
    return NULL;
}

/* ---- building server frames -------------------------------------------- */

struct pkt {
    uint8_t b[8192];
    size_t n;
};

static void p8(struct pkt *p, unsigned v) { p->b[p->n++] = (uint8_t)v; }
static void p16(struct pkt *p, unsigned v) { p8(p, v >> 8); p8(p, v); }
static void p32(struct pkt *p, uint32_t v) { p16(p, v >> 16); p16(p, v); }
static void pbytes(struct pkt *p, const void *d, size_t n) { memcpy(p->b + p->n, d, n); p->n += n; }
static void pstr(struct pkt *p, const char *s) { pbytes(p, s, strlen(s)); }
static void ptlv(struct pkt *p, unsigned t, const void *d, size_t n) { p16(p, t); p16(p, (unsigned)n); pbytes(p, d, n); }

static uint16_t server_seq = 100;

/* Feeds a FLAP to the engine in uneven pieces. */
static void feed_flap(int channel, const struct pkt *body)
{
    struct pkt f = { .n = 0 };
    p8(&f, '*');
    p8(&f, (unsigned)channel);
    p16(&f, server_seq++);
    p16(&f, (unsigned)body->n);
    pbytes(&f, body->b, body->n);
    static const size_t pieces[] = { 1, 3, 7, 2, 50, 1000 };
    size_t off = 0;
    for (int i = 0; off < f.n; i = (i + 1) % 6) {
        size_t k = pieces[i] < f.n - off ? pieces[i] : f.n - off;
        oscar_input(&o, f.b + off, k);
        off += k;
    }
}

static void snac_head(struct pkt *p, unsigned fam, unsigned sub, unsigned flags, uint32_t id)
{
    p16(p, fam);
    p16(p, sub);
    p16(p, flags);
    p32(p, id);
}

static void hello(void)
{
    struct pkt p = { .n = 0 };
    p32(&p, 1);
    feed_flap(1, &p);
}

static void user_info(struct pkt *p, const char *sn, unsigned cls, unsigned idle)
{
    p8(p, (unsigned)strlen(sn));
    pstr(p, sn);
    p16(p, 0);
    p16(p, idle ? 2 : 1);
    p16(p, 1); p16(p, 2); p16(p, cls);
    if (idle) {
        p16(p, 4); p16(p, 2); p16(p, idle);
    }
}

static void item(struct pkt *p, const char *name, unsigned gid, unsigned iid, unsigned cls, const uint16_t *order,
                 int n)
{
    p16(p, (unsigned)strlen(name));
    pstr(p, name);
    p16(p, gid);
    p16(p, iid);
    p16(p, cls);
    if (n < 0) {
        p16(p, 0);
        return;
    }
    p16(p, 4 + 2 * (unsigned)n);
    p16(p, 0xC8);
    p16(p, 2 * (unsigned)n);
    for (int i = 0; i < n; i++)
        p16(p, order[i]);
}

static void ack(unsigned status)
{
    struct pkt p = { .n = 0 };
    snac_head(&p, 0x13, 0x0E, 0, 0);
    p16(&p, status);
    feed_flap(2, &p);
}

/* ---- tests -------------------------------------------------------------- */

static void hex(const uint8_t *d, char *out)
{
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void test_md5(void)
{
    static const char *vec[][2] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a" },
    };
    for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        uint8_t d[16];
        char h[33];
        oscar_md5(vec[i][0], strlen(vec[i][0]), d);
        hex(d, h);
        if (strcmp(h, vec[i][1])) {
            fprintf(stderr, "md5(%s) = %s\n", vec[i][0], h);
            abort();
        }
    }
}

static void check_text(const char *html, const char *want)
{
    char out[256];
    oscar_html_to_text(out, sizeof(out), (const uint8_t *)html, strlen(html), 0);
    if (strcmp(out, want)) {
        fprintf(stderr, "html %s -> [%s], want [%s]\n", html, out, want);
        abort();
    }
}

static void test_text(void)
{
    char out[256];

    check_text("<HTML><BODY BGCOLOR=\"#ffffff\"><FONT FACE=\"Arial\">Hi &amp; <B>bye</B></FONT></BODY></HTML>",
               "Hi & bye");
    check_text("one<br>two<BR/>three", "one\ntwo\nthree");
    check_text("a   b\r\n c", "a b c");
    check_text("&lt;tag&gt; &quot;q&quot; &#65;&#x42; &bogus; &", "<tag> \"q\" AB &bogus; &");
    check_text("caf&#233; &#8364;", "caf\xE9 ?");
    check_text("\x93quoted\x94 \x96 it\x92s\x85 \x81", "\"quoted\" - it's\x84 ?");
    check_text("&#8220;hi&#8221; &#x2026;", "\"hi\" \x84");
    check_text("<unterminated", "");
    check_text("  <p>para</p><p>two</p>  ", "para\ntwo");
    check_text("x<br><br>y", "x\n\ny");
    /* UCS-2BE: "hé€" */
    static const uint8_t ucs[] = { 0, 'h', 0, 0xE9, 0x20, 0xAC, 0x20, 0x19, 0, 0x85 };
    oscar_html_to_text(out, sizeof(out), ucs, sizeof(ucs), 2);
    assert(!strcmp(out, "h\xE9?'?"));
    /* Truncation keeps the string terminated. */
    oscar_html_to_text(out, 4, (const uint8_t *)"abcdef", 6, 0);
    assert(!strcmp(out, "abc"));

    size_t n = oscar_text_to_html(out, sizeof(out), "a<b>&\"c\"\nd");
    assert(n == strlen(out));
    assert(!strcmp(out, "<HTML><BODY>a&lt;b&gt;&amp;&quot;c&quot;<BR>d</BODY></HTML>"));
    n = oscar_text_to_html(out, 40, "this text is far too long to fit in forty bytes");
    assert(n < 40 && n == strlen(out) && strstr(out, "</BODY></HTML>"));

    char norm[32];
    oscar_norm(norm, sizeof(norm), "Joe Smith 99");
    assert(!strcmp(norm, "joesmith99"));
    assert(oscar_same_name("JoeSmith", "joe smith"));
    assert(!oscar_same_name("joe", "joey"));
}

static void sign_on(void)
{
    const uint8_t *b;
    size_t n, vn;
    struct oscar_ops ops = { NULL, cap_send, cap_event };

    sent_off = sent_len = 0;
    nevents = 0;
    oscar_init(&o, &ops);
    oscar_tick(&o, 1000);
    oscar_login(&o, "Glaze User", "secret");
    hello();
    assert(next_flap(&b, &n) == 1 && n == 4 && be16(b + 2) == 1);
    b = expect_snac(0x17, 0x06, &n);
    const uint8_t *v = tlv(b, n, 1, &vn);
    assert(v && vn == 10 && !memcmp(v, "Glaze User", 10));
    assert(find_event(OSCAR_EV_PROGRESS) >= 0);

    /* Key "1234": the hash is md5(key + md5(password) + AIM string). */
    struct pkt p = { .n = 0 };
    snac_head(&p, 0x17, 0x07, 0, 1);
    p16(&p, 4);
    pstr(&p, "1234");
    feed_flap(2, &p);
    b = expect_snac(0x17, 0x02, &n);
    v = tlv(b, n, 0x25, &vn);
    assert(v && vn == 16);
    uint8_t ph[16], buf[64], want[16];
    oscar_md5("secret", 6, ph);
    memcpy(buf, "1234", 4);
    memcpy(buf + 4, ph, 16);
    memcpy(buf + 20, "AOL Instant Messenger (SM)", 26);
    oscar_md5(buf, 46, want);
    assert(!memcmp(v, want, 16));
    assert(tlv(b, n, 0x4C, &vn) && vn == 0);
    assert(tlv(b, n, 0x4A, &vn) && vn == 1);

    p.n = 0;
    snac_head(&p, 0x17, 0x03, 0, 2);
    ptlv(&p, 1, "glazeuser", 9);
    ptlv(&p, 5, "10.0.2.2:5191", 13);
    ptlv(&p, 6, "COOKIE-BYTES", 12);
    feed_flap(2, &p);
    assert(o.state == OSCAR_REDIRECTED && find_event(OSCAR_EV_REDIRECT) >= 0);
    assert(!strcmp(o.bos_host, "10.0.2.2") && o.bos_port == 5191);
    assert(!o.password[0]);
    expect_nothing_sent();

    oscar_bos_connected(&o);
    hello();
    assert(next_flap(&b, &n) == 1 && n == 4 + 4 + 12 && be16(b + 4) == 6 && !memcmp(b + 8, "COOKIE-BYTES", 12));

    p.n = 0;
    snac_head(&p, 1, 3, 0, 0);
    p16(&p, 1); p16(&p, 2); p16(&p, 3); p16(&p, 4); p16(&p, 0x13); p16(&p, 0x0B);
    feed_flap(2, &p);
    b = expect_snac(1, 0x17, &n);
    assert(n == 20 && be16(b) == 1 && be16(b + 2) == 4);

    p.n = 0;
    snac_head(&p, 1, 0x18, 0, 0);
    feed_flap(2, &p);
    expect_snac(1, 6, NULL);

    p.n = 0;
    snac_head(&p, 1, 7, 0, 0);
    p16(&p, 2);
    for (unsigned id = 1; id <= 2; id++) {
        p16(&p, id);
        for (int i = 0; i < 8; i++)
            p32(&p, 1000 + id);
        p8(&p, 0);
    }
    p16(&p, 1); p16(&p, 0);     /* a group section, which is ignored */
    feed_flap(2, &p);
    b = expect_snac(1, 8, &n);
    assert(n == 4 && be16(b) == 1 && be16(b + 2) == 2);
    expect_snac(1, 0x0E, NULL);
    expect_snac(2, 2, NULL);
    expect_snac(3, 2, NULL);
    expect_snac(4, 4, NULL);
    expect_snac(0x13, 2, NULL);
    expect_snac(0x13, 4, NULL);
    expect_nothing_sent();

    /* The list in two replies. Root order: Family (2), Buddies (1);
     * Buddies' order: bob (2), Alice (1). A buddy in a missing group and a
     * permit item are dropped. */
    uint16_t root[] = { 2, 1 }, buddies[] = { 2, 1 }, family[] = { 3 };
    p.n = 0;
    snac_head(&p, 0x13, 6, 1, 0);
    p8(&p, 0);
    p16(&p, 3);
    item(&p, "", 0, 0, 1, root, 2);
    item(&p, "Alice", 1, 1, 0, NULL, -1);
    item(&p, "Buddies", 1, 0, 1, buddies, 2);
    feed_flap(2, &p);
    expect_nothing_sent();
    p.n = 0;
    snac_head(&p, 0x13, 6, 0, 0);
    p8(&p, 0);
    p16(&p, 5);
    item(&p, "bob", 1, 2, 0, NULL, -1);
    item(&p, "Family", 2, 0, 1, family, 1);
    item(&p, "Mom", 2, 3, 0, NULL, -1);
    item(&p, "lost", 9, 4, 0, NULL, -1);
    item(&p, "blocked", 0, 7, 3, NULL, -1);
    p32(&p, 12345);
    feed_flap(2, &p);
    expect_snac(0x13, 7, NULL);
    b = expect_snac(4, 2, &n);
    assert(n == 16 && (b[5] & 0x08));
    expect_snac(2, 4, NULL);
    b = expect_snac(1, 2, &n);
    assert(n == 5 * 8);
    expect_nothing_sent();
    assert(o.state == OSCAR_ONLINE && find_event(OSCAR_EV_ONLINE) >= 0);

    assert(o.ngroups == 2 && !strcmp(o.groups[0].name, "Family") && !strcmp(o.groups[1].name, "Buddies"));
    assert(o.nbuddies == 3);
    assert(!strcmp(o.buddies[0].name, "Mom") && !strcmp(o.buddies[1].name, "bob") &&
           !strcmp(o.buddies[2].name, "Alice"));
    assert(o.max_iid == 7 && o.max_gid == 9);
}

static void test_session(void)
{
    const uint8_t *b;
    size_t n;
    struct pkt p = { .n = 0 };

    sign_on();

    /* Presence: Alice away and idle, Mom online; then Mom leaves. */
    nevents = 0;
    snac_head(&p, 3, 0x0B, 0, 0);
    user_info(&p, "alice", 0x0010 | 0x0020, 5);
    user_info(&p, "M o m", 0x0010, 0);
    feed_flap(2, &p);
    struct oscar_buddy *a = oscar_find_buddy(&o, "ALICE"), *m = oscar_find_buddy(&o, "mom");
    assert(a && a->online && a->away && a->idle_min == 5);
    assert(m && m->online && !m->away);
    assert(find_event(OSCAR_EV_BUDDIES) >= 0);
    oscar_tick(&o, 1000 + 3 * 60000);
    assert(oscar_idle_minutes(&o, a) == 8);
    p.n = 0;
    snac_head(&p, 3, 0x0C, 0, 0);
    user_info(&p, "mom", 0, 0);
    feed_flap(2, &p);
    assert(!m->online);
    /* Three minutes passed: a keepalive went out on the tick. */
    const uint8_t *kb;
    assert(next_flap(&kb, &n) == 5 && n == 0);
    expect_nothing_sent();

    /* An IM with formatting, and the auto-response flag. */
    nevents = 0;
    p.n = 0;
    snac_head(&p, 4, 7, 0, 0);
    pstr(&p, "COOKIE01");
    p16(&p, 1);
    user_info(&p, "Alice", 0x10, 0);
    static const char html[] = "<HTML><BODY>Hi &amp; <b>bye</b></BODY></HTML>";
    p16(&p, 2);
    p16(&p, (unsigned)(4 + 1 + 4 + 4 + strlen(html)));
    p8(&p, 5); p8(&p, 1); p16(&p, 1); p8(&p, 1);
    p8(&p, 1); p8(&p, 1); p16(&p, (unsigned)(4 + strlen(html)));
    p16(&p, 0); p16(&p, 0);
    pstr(&p, html);
    ptlv(&p, 4, NULL, 0);
    feed_flap(2, &p);
    int e = find_event(OSCAR_EV_IM);
    assert(e >= 0 && !strcmp(events[e].name, "Alice") && !strcmp(events[e].text, "Hi & bye") && events[e].flag == 1);

    /* UCS-2 text, and no auto-response. */
    nevents = 0;
    p.n = 0;
    snac_head(&p, 4, 7, 0, 0);
    pstr(&p, "COOKIE02");
    p16(&p, 1);
    user_info(&p, "bob", 0x10, 0);
    p16(&p, 2);
    p16(&p, 4 + 4 + 6);
    p8(&p, 1); p8(&p, 1); p16(&p, 4 + 6);
    p16(&p, 2); p16(&p, 0);
    p16(&p, 'y'); p16(&p, 'o'); p16(&p, 0x263A);
    feed_flap(2, &p);
    e = find_event(OSCAR_EV_IM);
    assert(e >= 0 && !strcmp(events[e].text, "yo?") && events[e].flag == 0);

    /* Typing. */
    nevents = 0;
    p.n = 0;
    snac_head(&p, 4, 0x14, 0, 0);
    pbytes(&p, "\0\0\0\0\0\0\0\0", 8);
    p16(&p, 1);
    p8(&p, 3);
    pstr(&p, "bob");
    p16(&p, 2);
    feed_flap(2, &p);
    e = find_event(OSCAR_EV_TYPING);
    assert(e >= 0 && !strcmp(events[e].name, "bob") && events[e].flag == 2);

    /* Sending: the text fragment carries the escaped HTML. */
    assert(oscar_send_im(&o, "bob", "1 < 2", false) == 0);
    b = expect_snac(4, 6, &n);
    assert(be16(b + 8) == 1 && b[10] == 3 && !memcmp(b + 11, "bob", 3));
    size_t vn;
    const uint8_t *v = tlv(b + 14, n - 14, 2, &vn);
    assert(v && v[0] == 5 && v[5] == 1);
    unsigned flen = be16(v + 7);
    assert(be16(v + 9) == 0 && !memcmp(v + 13, "<HTML><BODY>1 &lt; 2</BODY></HTML>", flen - 4));
    assert(!tlv(b + 14, n - 14, 4, &vn));
    uint32_t reqid = (uint32_t)b[-4] << 24 | (uint32_t)b[-3] << 16 | (uint32_t)b[-2] << 8 | b[-1];

    /* The server refuses it: the error names the recipient. */
    nevents = 0;
    p.n = 0;
    snac_head(&p, 4, 1, 0, reqid);
    p16(&p, 4);
    feed_flap(2, &p);
    e = find_event(OSCAR_EV_NOT_SENT);
    assert(e >= 0 && !strcmp(events[e].name, "bob") && strstr(events[e].text, "not signed on"));

    assert(oscar_send_typing(&o, "bob", 1) == 0);
    b = expect_snac(4, 0x14, &n);
    assert(n == 8 + 2 + 1 + 3 + 2 && be16(b + 14) == 1);

    /* Away on and off. */
    assert(oscar_set_away(&o, "Out to lunch") == 0);
    b = expect_snac(2, 4, &n);
    v = tlv(b, n, 4, &vn);
    assert(v && vn > 12 && memmem(v, vn, "Out to lunch", 12));
    assert(oscar_set_away(&o, NULL) == 0);
    b = expect_snac(2, 4, &n);
    assert(tlv(b, n, 4, &vn) && vn == 0);

    /* Info query and reply. */
    assert(oscar_get_info(&o, "Alice") == 0);
    b = expect_snac(2, 0x15, &n);
    assert(be16(b + 2) == 3 && b[4] == 5 && !memcmp(b + 5, "Alice", 5));
    nevents = 0;
    p.n = 0;
    snac_head(&p, 2, 6, 0, 0);
    user_info(&p, "Alice", 0x30, 0);
    ptlv(&p, 1, "text/aolrtf; charset=\"us-ascii\"", 31);
    ptlv(&p, 2, "<b>About</b> me", 15);
    ptlv(&p, 3, "text/aolrtf; charset=\"us-ascii\"", 31);
    ptlv(&p, 4, "brb", 3);
    feed_flap(2, &p);
    e = find_event(OSCAR_EV_INFO);
    assert(e >= 0 && !strcmp(events[e].name, "Alice") && !strcmp(events[e].text, "About me") &&
           !strcmp(events[e].text2, "brb"));
    expect_nothing_sent();
}

static void test_edits(void)
{
    const uint8_t *b;
    size_t n;

    sign_on();

    /* A buddy in a new group: transaction, group, root, buddy, group order. */
    nevents = 0;
    assert(oscar_add_buddy(&o, "Carol", "Work") == 0);
    expect_snac(0x13, 0x11, NULL);
    b = expect_snac(0x13, 0x08, &n);
    assert(be16(b) == 4 && !memcmp(b + 2, "Work", 4) && be16(b + 6) == 10 && be16(b + 8) == 0 && be16(b + 10) == 1);
    b = expect_snac(0x13, 0x09, &n);
    assert(be16(b) == 0 && be16(b + 6) == 1 && be16(b + 8) == 4 + 6 && be16(b + 10) == 0xC8 && be16(b + 14) == 2 &&
           be16(b + 16) == 1 && be16(b + 18) == 10);
    b = expect_snac(0x13, 0x08, &n);
    assert(be16(b) == 5 && !memcmp(b + 2, "Carol", 5) && be16(b + 7) == 10 && be16(b + 9) == 8 && be16(b + 11) == 0);
    b = expect_snac(0x13, 0x09, &n);
    assert(!memcmp(b + 2, "Work", 4) && be16(b + 14) == 0xC8 && be16(b + 16) == 2 && be16(b + 18) == 8);
    expect_snac(0x13, 0x12, NULL);
    expect_nothing_sent();
    assert(o.ngroups == 3 && o.nbuddies == 4 && !strcmp(o.buddies[3].name, "Carol"));
    for (int i = 0; i < 4; i++)
        ack(0);
    int e = find_event(OSCAR_EV_EDITED);
    assert(e >= 0 && events[e].flag == 0 && o.edits_pending == 0);

    /* Already there. */
    assert(oscar_add_buddy(&o, "car ol", "work") < 0);
    expect_nothing_sent();

    /* Into an existing group, which keeps the list grouped. */
    assert(oscar_add_buddy(&o, "Dad", "Family") == 0);
    expect_snac(0x13, 0x11, NULL);
    expect_snac(0x13, 0x08, NULL);
    b = expect_snac(0x13, 0x09, &n);
    assert(!memcmp(b + 2, "Family", 6) && be16(b + 18) == 4 && be16(b + 20) == 3 && be16(b + 22) == 9);
    expect_snac(0x13, 0x12, NULL);
    assert(!strcmp(o.buddies[1].name, "Dad") && !strcmp(o.buddies[2].name, "bob"));
    ack(0);
    ack(0);

    /* Removing bob updates Buddies' order to Alice alone. */
    assert(oscar_remove_buddy(&o, "BOB") == 0);
    expect_snac(0x13, 0x11, NULL);
    b = expect_snac(0x13, 0x0A, &n);
    assert(be16(b) == 3 && !memcmp(b + 2, "bob", 3));
    b = expect_snac(0x13, 0x09, &n);
    assert(!memcmp(b + 2, "Buddies", 7) && be16(b + 19) == 2 && be16(b + 21) == 1);
    expect_snac(0x13, 0x12, NULL);
    assert(!oscar_find_buddy(&o, "bob"));

    /* A refused edit fetches the list again, and the reply replaces ours. */
    nevents = 0;
    ack(0x0A);
    expect_snac(0x13, 0x04, NULL);
    e = find_event(OSCAR_EV_EDITED);
    assert(e >= 0 && events[e].flag < 0);
    struct pkt p = { .n = 0 };
    uint16_t root[] = { 1 }, order[] = { 1, 2 };
    snac_head(&p, 0x13, 6, 0, 0);
    p8(&p, 0);
    p16(&p, 4);
    item(&p, "", 0, 0, 1, root, 1);
    item(&p, "Buddies", 1, 0, 1, order, 2);
    item(&p, "Alice", 1, 1, 0, NULL, -1);
    item(&p, "bob", 1, 2, 0, NULL, -1);
    p32(&p, 0);
    feed_flap(2, &p);
    assert(o.ngroups == 1 && o.nbuddies == 2 && oscar_find_buddy(&o, "bob"));
    expect_nothing_sent();

    /* Another session adds a buddy; then removes the group. */
    p.n = 0;
    snac_head(&p, 0x13, 8, 0, 0);
    item(&p, "Eve", 1, 30, 0, NULL, -1);
    feed_flap(2, &p);
    assert(oscar_find_buddy(&o, "eve") && o.max_iid >= 30);
    assert(oscar_remove_group(&o, "buddies") == 0);
    expect_snac(0x13, 0x11, NULL);
    for (int i = 0; i < 3; i++)
        expect_snac(0x13, 0x0A, NULL);
    b = expect_snac(0x13, 0x0A, &n);
    assert(!memcmp(b + 2, "Buddies", 7));
    b = expect_snac(0x13, 0x09, &n);
    assert(be16(b) == 0 && be16(b + 8) == 4 && be16(b + 12) == 0);
    expect_snac(0x13, 0x12, NULL);
    assert(o.ngroups == 0 && o.nbuddies == 0);
}

static void test_framing(void)
{
    struct pkt p = { .n = 0 };

    sign_on();
    /* An oversized FLAP is skipped whole; the next one still parses. */
    uint8_t big[6] = { '*', 2, 0, 1, 0xFF, 0xF0 };
    oscar_input(&o, big, 6);
    static uint8_t junk[0xFFF0];
    memset(junk, 'x', sizeof(junk));
    oscar_input(&o, junk, sizeof(junk));
    nevents = 0;
    snac_head(&p, 3, 0x0B, 0, 0);
    user_info(&p, "bob", 0x10, 0);
    feed_flap(2, &p);
    assert(oscar_find_buddy(&o, "bob")->online);

    /* Signed on elsewhere. */
    nevents = 0;
    p.n = 0;
    uint8_t code[2] = { 0, 1 };
    ptlv(&p, 9, code, 2);
    feed_flap(4, &p);
    int e = find_event(OSCAR_EV_SIGNED_OFF);
    assert(e >= 0 && strstr(events[e].text, "another location") && o.state == OSCAR_IDLE);
    assert(oscar_send_im(&o, "bob", "hi", false) < 0);

    /* A bad password. */
    struct oscar_ops ops = { NULL, cap_send, cap_event };
    oscar_init(&o, &ops);
    sent_off = sent_len = 0;
    nevents = 0;
    oscar_login(&o, "x", "y");
    hello();
    p.n = 0;
    snac_head(&p, 0x17, 0x07, 0, 1);
    p16(&p, 0);
    feed_flap(2, &p);
    p.n = 0;
    snac_head(&p, 0x17, 0x03, 0, 2);
    uint8_t err[2] = { 0, 5 };
    ptlv(&p, 8, err, 2);
    feed_flap(2, &p);
    e = find_event(OSCAR_EV_FAILED);
    assert(e >= 0 && strstr(events[e].text, "password") && o.state == OSCAR_FAILED);

    /* Not AIM at all. */
    oscar_init(&o, &ops);
    nevents = 0;
    oscar_login(&o, "x", "y");
    oscar_input(&o, "HTTP/1.1 400", 12);
    assert(find_event(OSCAR_EV_FAILED) >= 0);
}

/* ---- live --------------------------------------------------------------- */

static int sock = -1;

static int sock_send(void *ctx, const void *buf, size_t len)
{
    return send(sock, buf, len, 0) == (ssize_t)len ? 0 : -1;
}

static void live_event(void *ctx, const struct oscar_event *ev)
{
    static const char *names[] = { "PROGRESS", "REDIRECT", "ONLINE", "FAILED", "BUDDIES", "IM", "TYPING",
                                   "INFO", "NOT_SENT", "EDITED", "SIGNED_OFF" };
    printf("event %s name=%s text=%s text2=%s flag=%d\n", names[ev->type], ev->name ? ev->name : "",
           ev->text ? ev->text : "", ev->text2 ? ev->text2 : "", ev->flag);
    if (ev->type == OSCAR_EV_BUDDIES)
        for (int g = 0; g < o.ngroups; g++) {
            printf("  [%s]\n", o.groups[g].name);
            for (int i = 0; i < o.nbuddies; i++)
                if (o.buddies[i].gid == o.groups[g].gid)
                    printf("    %s%s%s\n", o.buddies[i].name, o.buddies[i].online ? " (online)" : "",
                           o.buddies[i].away ? " (away)" : "");
        }
    if (ev->type == OSCAR_EV_IM && !strstr(ev->text, "echo"))
        oscar_send_im(&o, ev->name, "echo from aim-test", false);
}

static int dial(const char *host, unsigned port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    char ps[8];

    snprintf(ps, sizeof(ps), "%u", port);
    if (getaddrinfo(host, ps, &hints, &res))
        return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
}

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static int live(int argc, char **argv)
{
    struct oscar_ops ops = { NULL, sock_send, live_event };
    const char *to = argc > 6 ? argv[6] : NULL, *msg = argc > 7 ? argv[7] : "hello from aim-test";
    bool sent_im = false, edited = false;

    oscar_init(&o, &ops);
    sock = dial(argv[2], (unsigned)atoi(argv[3]));
    if (sock < 0) {
        perror("connect");
        return 1;
    }
    oscar_tick(&o, now_ms());
    oscar_login(&o, argv[4], argv[5]);
    uint32_t start = now_ms();
    uint32_t secs = getenv("AIM_SECS") ? (uint32_t)atoi(getenv("AIM_SECS")) : 20;
    while (now_ms() - start < secs * 1000) {
        struct pollfd pf = { sock, POLLIN, 0 };
        uint8_t buf[4096];
        if (poll(&pf, 1, 100) > 0) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                close(sock);
                oscar_closed(&o);
                if (o.state != OSCAR_REDIRECTED)
                    return o.state == OSCAR_FAILED;
                sock = dial(o.bos_host, o.bos_port);
                if (sock < 0) {
                    printf("BOS connect to %s:%u failed\n", o.bos_host, o.bos_port);
                    return 1;
                }
                oscar_bos_connected(&o);
                continue;
            }
            oscar_input(&o, buf, (size_t)n);
            if (o.state == OSCAR_REDIRECTED) {
                close(sock);
                sock = dial(o.bos_host, o.bos_port);
                if (sock < 0) {
                    printf("BOS connect to %s:%u failed\n", o.bos_host, o.bos_port);
                    return 1;
                }
                oscar_bos_connected(&o);
            }
        }
        oscar_tick(&o, now_ms());
        if (o.state == OSCAR_FAILED)
            return 1;
        if (o.state == OSCAR_ONLINE && !edited) {
            edited = true;
            oscar_set_profile(&o, "Glaze AIM test profile");
            oscar_add_buddy(&o, "glazetestbuddy", "Glaze Test");
            if (to) {
                oscar_get_info(&o, to);
                if (!oscar_find_buddy(&o, to))
                    oscar_add_buddy(&o, to, "Buddies");
            }
        }
        if (o.state == OSCAR_ONLINE && to && !sent_im && now_ms() - start > 2000) {
            sent_im = true;
            oscar_send_im(&o, to, msg, false);
            printf("sent IM to %s\n", to);
        }
        if (o.state == OSCAR_ONLINE && edited && o.edits_pending == 0 && oscar_find_buddy(&o, "glazetestbuddy") &&
            now_ms() - start > 4000) {
            oscar_remove_group(&o, "Glaze Test");
        }
    }
    printf("done: %d groups, %d buddies\n", o.ngroups, o.nbuddies);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 5 && !strcmp(argv[1], "--live"))
        return live(argc, argv);
    test_md5();
    test_text();
    test_session();
    sent_off = sent_len = 0;
    test_edits();
    sent_off = sent_len = 0;
    test_framing();
    printf("aim: ALL PASS\n");
    return 0;
}
