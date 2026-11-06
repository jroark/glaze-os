/* The research script runner against a fake card and an in-memory file
 * system: request encodings, probe skipping the key tokens, log pieces,
 * the upload key reaching only its file, and stopping at the first error. */
#include <glaze/fs.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define GLAZE_SYS_H
void gz_sleep_ms(uint32_t ms);
uint32_t gz_time_ms(void);
#include "../../apps/eyefi/research.c"

void gz_sleep_ms(uint32_t ms) { (void)ms; }
uint32_t gz_time_ms(void) { return 1234; }

/* ---- fake card ---- */
static uint8_t mail[4][EYEFI_SIZE], last_req[EYEFI_REQUEST_MAX];
static unsigned requests, tokens_seen[256];
static const char key[] = "SECRETKEY0123456";
static int mrd(void *c, unsigned f, uint8_t *b) { (void)c; memcpy(b, mail[f], EYEFI_SIZE); return 0; }
static int mwr(void *c, unsigned f, const uint8_t *b)
{
    (void)c;
    memcpy(mail[f], b, EYEFI_SIZE);
    if (f != EYEFI_REQC) return 0;
    const uint8_t *q = mail[EYEFI_REQM];
    uint8_t *r = mail[EYEFI_RSPM];
    memcpy(last_req, q, sizeof(last_req));
    ++requests;
    if (q[0] == 'o' && q[1] == 0x42) return 0;          /* never answers */
    memset(r, 0, EYEFI_SIZE);
    if (q[0] == 'o') {
        ++tokens_seen[q[1]];
        if (q[1] == 0xfd) { r[0] = 16; memcpy(r + 1, key, 16); }
        else if (q[1] == 7) { r[0] = 4; r[3] = 0x80; }   /* 32768 */
        else { r[0] = 1; r[1] = q[1]; }
    } else if (q[0] == 'm') memset(r, 'L', EYEFI_SIZE);
    else r[0] = 1;
    memcpy(mail[EYEFI_RSPC], mail[EYEFI_REQC], 4);
    return 0;
}
static void msleep(void *c, unsigned ms) { (void)c; (void)ms; }

/* ---- in-memory file system ---- */
struct file { char path[FS_PATH_MAX]; uint8_t *data; unsigned size, pos; int used; };
static struct file files[400];
static int open_fds[8];
static struct file *find(const char *p, int create)
{
    for (unsigned i = 0; i < 400; ++i)
        if (files[i].used && !strcmp(files[i].path, p)) return &files[i];
    if (!create) return NULL;
    for (unsigned i = 0; i < 400; ++i)
        if (!files[i].used) {
            files[i].used = 1; snprintf(files[i].path, FS_PATH_MAX, "%s", p);
            files[i].data = NULL; files[i].size = 0; return &files[i];
        }
    assert(0); return NULL;
}
int fs_mkdirs(const char *p) { (void)p; return 0; }
int fs_open(const char *p, int flags)
{
    struct file *f = find(p, flags & FS_O_CREATE);
    if (!f) return GZ_ENOENT;
    if (flags & FS_O_TRUNC) f->size = 0;
    f->pos = flags & FS_O_APPEND ? f->size : 0;
    for (int i = 0; i < 8; ++i) if (!open_fds[i]) { open_fds[i] = (int)(f - files) + 1; return i; }
    assert(0); return -1;
}
int fs_read(int fd, void *b, uint32_t n)
{
    struct file *f = &files[open_fds[fd] - 1];
    if (n > f->size - f->pos) n = f->size - f->pos;
    memcpy(b, f->data + f->pos, n); f->pos += n; return (int)n;
}
int fs_write(int fd, const void *b, uint32_t n)
{
    struct file *f = &files[open_fds[fd] - 1];
    assert(n <= FS_IO_MAX);
    if (f->pos + n > f->size) { f->data = realloc(f->data, f->pos + n); f->size = f->pos + n; }
    memcpy(f->data + f->pos, b, n); f->pos += n; return (int)n;
}
int fs_remove(const char *p)
{
    struct file *f = find(p, 0);
    if (!f) return GZ_ENOENT;
    free(f->data); f->used = 0; return 0;
}
int fs_close(int fd) { assert(open_fds[fd]); open_fds[fd] = 0; return 0; }

static char out[65536];
static int run(const char *text)
{
    static char script[4096];
    snprintf(script, sizeof(script), "%s", text);
    static uint8_t buf[EYEFI_SIZE];
    const struct eyefi_io io = {NULL, mrd, mwr, msleep};
    fflush(stdout);
    int console = dup(1);
    FILE *saved = freopen("eyefi-research-test.out", "w", stdout);
    assert(saved && console >= 0);
    int r = eyefi_research(&io, buf, script);
    fflush(stdout);
    dup2(console, 1); close(console);
    FILE *f = fopen("eyefi-research-test.out", "r");
    size_t n = fread(out, 1, sizeof(out) - 1, f); out[n] = 0; fclose(f);
    remove("eyefi-research-test.out");
    assert(!memmem(buf, sizeof(buf), key, 16));
    return r;
}

int main(void)
{
    /* Probe skips the card key (3) and upload key (0xfd); 0x42 times out. */
    assert(run("probe 0 255\n") == 0);
    assert(!tokens_seen[3] && !tokens_seen[0xfd] && tokens_seen[0] && tokens_seen[0xff]);
    struct file *p = find(RESEARCH_DIR "/probe-00-ff.bin", 0);
    assert(p && p->size > 3 && p->data[0] == 0 && p->data[3] == 1);
    assert(strstr(out, "o 42: no response") && strstr(out, "saved " RESEARCH_DIR "/probe-00-ff.bin"));

    /* Network add: 67-byte net_request with the hex key, key not echoed. */
    char line[160] = "addnet ";
    for (unsigned i = 0; i < 32; ++i) snprintf(line + strlen(line), 3, "%02x", i + 0xa0);
    strcat(line, " My Net\n");
    assert(run(line) == 0);
    assert(last_req[0] == 'a' && last_req[1] == 6 && !memcmp(last_req + 2, "My Net", 6));
    assert(last_req[34] == 32 && last_req[35] == 0xa0 && last_req[66] == 0xbf);
    assert(!strstr(out, "a0a1") && strstr(out, "key elided"));

    /* Config writes, raw, WLAN and log. */
    assert(run("setstr 6 10.0.0.2\nset 0x24 ff\nwlan on\nraw 4f0a01\nlog\n") == 0);
    assert(find(RESEARCH_DIR "/000-O06.bin", 0) && find(RESEARCH_DIR "/001-O24.bin", 0));
    assert(last_req[0] == 'm' && last_req[3] == 0x40 && last_req[4] == 0);   /* offset 16384 */
    assert(find(RESEARCH_DIR "/log-000000.bin", 0)->size == EYEFI_SIZE);
    assert(find(RESEARCH_DIR "/log-004000.bin", 0));

    /* Upload key goes only to its file. */
    assert(run("uploadkey\n") == 0);
    struct file *k = find(RESEARCH_DIR "/uploadkey.bin", 0);
    assert(k && k->size == 16 && !memcmp(k->data, key, 16));
    assert(!strstr(out, "SECRET") && !strstr(out, "53 45 43"));
    assert(run("forget-key\nforget-key\n") == 0 && !find(RESEARCH_DIR "/uploadkey.bin", 0));

    /* Drop copies into DCIM. */
    struct file *src = find("/Documents/a.jpg", 1);
    src->data = malloc(5000); memset(src->data, 7, 5000); src->size = 5000;
    assert(run("drop /Documents/a.jpg TEST0001.JPG\n") == 0);
    assert(find(DROP_DIR "/TEST0001.JPG", 0)->size == 5000);

    /* Refusals and stopping at the first failure. */
    unsigned before = requests;
    assert(run("info 3\nscan\n") == GZ_EINVAL && requests == before);
    assert(run("info 0xfd\n") == GZ_EINVAL);
    assert(run("bogus\nscan\n") == GZ_EINVAL && requests == before);
    assert(run("drop /Documents/a.jpg ../x\n") == GZ_EINVAL);
    assert(run("addnet 0011 Net\n") == GZ_EINVAL);
    assert(run("# comment\n\nscan\r\n") == 0 && requests == before + 1);
    puts("eyefi: research runner PASS");
    return 0;
}
