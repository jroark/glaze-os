/*
 * Host renderer for the browser engine (apps/browser): loads a page with
 * the real loader (HTTP over the host's sockets, file paths, res: from
 * assets/web) and writes the whole laid-out document to a BMP, 240 px wide.
 *
 * usage: browse <url | file.html> [out.bmp]     BROWSE_DUMP=1 prints the styled tree,
 * BROWSE_ROOTS=roots.der replaces the trusted roots (concatenated DER certificates)
 */
#include "hostres.h"
#include "../apps/browser/load.h"
#include "../apps/browser/script.h"
#include "../apps/browser/net.h"
#include "../apps/browser/plat.h"

#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_H   8000

/* ---- platform ---------------------------------------------------------------- */

uint32_t plat_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int read_all(const char *path, uint8_t **data, int *len)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return WE_NOTFOUND;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = malloc((size_t)n + 1);
    if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return WE_NOTFOUND;
    }
    fclose(f);
    *len = (int)n;
    return 0;
}

int plat_read_file(const char *path, uint8_t **data, int *len)
{
    return read_all(path, data, len);
}

/* Match the device's immutable resource lifetime, then release backing files
 * on exit so frame previews also run cleanly under LeakSanitizer. */
static struct resource_buffer {
    struct resource_buffer *next;
    uint8_t *data;
} *resource_buffers;
static void free_resources(void)
{
    while(resource_buffers){
        struct resource_buffer *r=resource_buffers;
        resource_buffers=r->next;free(r->data);free(r);
    }
}

const void *plat_res(const char *name, uint32_t *size)
{
    char path[256];
    uint8_t *d;
    int n;

    snprintf(path, sizeof(path), "../assets/web/%s", name);
    if (read_all(path, &d, &n))
        return NULL;
    struct resource_buffer *r=malloc(sizeof *r);
    if(!r){free(d);return NULL;}
    if(!resource_buffers && atexit(free_resources)){free(r);free(d);return NULL;}
    r->data=d;r->next=resource_buffers;resource_buffers=r;
    *size = (uint32_t)n;
    return d;
}

/* Synchronous lookups: the answer waits in a slot for the poll. */
static struct {
    bool used;
    int rv;
    uint32_t addr;
} lookups[8];

int plat_resolve_start(const char *host)
{
    struct addrinfo hints, *res;

    for (int i = 0; i < 8; i++) {
        if (lookups[i].used)
            continue;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        lookups[i].used = true;
        lookups[i].rv = getaddrinfo(host, NULL, &hints, &res) ? WE_DNS : 1;
        if (lookups[i].rv == 1) {
            lookups[i].addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
        }
        return i;
    }
    return WE_NOMEM;
}

int plat_resolve_poll(int id, uint32_t *addr)
{
    lookups[id].used = false;
    *addr = lookups[id].addr;
    return lookups[id].rv;
}

void plat_resolve_cancel(int id)
{
    lookups[id].used = false;
}

const void *plat_roots(uint32_t *size)
{
    static uint8_t *roots;
    static int n;
    const char *path = getenv("BROWSE_ROOTS");

    if (!roots && read_all(path ? path : "../assets/ssl/roots.der", &roots, &n))
        return NULL;
    *size = (uint32_t)n;
    return roots;
}

long long plat_time(void)
{
    return (long long)time(NULL);
}

void plat_noise(uint8_t *buf, int n)
{
    FILE *f = fopen("/dev/urandom", "rb");

    if (!f || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "browse: no /dev/urandom\n");
        exit(1);
    }
    fclose(f);
}

void plat_load_seed(uint8_t *seed)
{
    (void)seed;
}

void plat_save_seed(const uint8_t *seed)
{
    (void)seed;
}

/* Opt-in directory; no persistent writes from previews/tests by default. */
int plat_storage_read(int slot, void *data, int cap)
{
    const char *dir=getenv("BROWSE_STORAGE");
    if (!dir) return 0;
    char path[1024];snprintf(path,sizeof path,"%s/storage%d",dir,slot);
    FILE *f=fopen(path,"rb");if(!f)return 0;
    int n=(int)fread(data,1,(size_t)cap,f);
    int extra=fgetc(f), err=ferror(f);fclose(f);
    return extra!=EOF || err ? -1 : n;
}
int plat_storage_write(int slot, const void *data, int len)
{
    const char *dir=getenv("BROWSE_STORAGE");
    if (!dir) return -1;
    char path[1024],tmp[1024];
    snprintf(path,sizeof path,"%s/storage%d",dir,slot);
    snprintf(tmp,sizeof tmp,"%s/storage%d.tmp",dir,slot);
    FILE *f=fopen(tmp,"wb");if(!f)return -1;
    bool ok=fwrite(data,1,(size_t)len,f)==(size_t)len;
    if(fclose(f))ok=false;
    if(!ok || rename(tmp,path)) { remove(tmp);return -1; }
    return 0;
}

/* ---- debugging ---------------------------------------------------------------- */

static void dump(const struct node *n, int depth)
{
    for (const struct node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            printf("%*s\"%.*s\"\n", depth * 2, "", c->len > 40 ? 40 : c->len, c->text);
            continue;
        }
        const struct style *st = c->st;
        if (!st) {
            printf("%*s<%s> (unstyled)\n", depth * 2, "", c->name);
            continue;
        }
        printf("%*s<%s> d%d font %d m %d/%d/%d/%d p %d/%d/%d/%d w %d:%d bg %06x\n", depth * 2, "", c->name,
               st->display, st->font_css / 4, st->margin[0].v, st->margin[1].v, st->margin[2].v, st->margin[3].v,
               st->padding[0].v, st->padding[1].v, st->padding[2].v, st->padding[3].v, st->width.unit, st->width.v,
               st->bg & 0xFFFFFF);
        if (st->display != D_NONE)
            dump(c, depth + 1);
    }
}

int main(int argc, char **argv)
{
    static uint16_t px[240 * MAX_H];
    static struct load ld;
    const char *arg = argc > 1 ? argv[1] : "../tests/web/basic.html";
    const char *out = argc > 2 ? argv[2] : "browse.bmp";
    char url[1024];
    struct page *page = NULL;

    signal(SIGPIPE, SIG_IGN);
    if (strstr(arg, ":"))
        snprintf(url, sizeof(url), "%s", arg);
    else {
        char abs[PATH_MAX];
        snprintf(url, sizeof(url), "file://%s", realpath(arg, abs) ? abs : arg);
    }

    uint32_t t0 = plat_ms();
    load_begin(&ld, url, NULL, NULL, false, 240);
    for (;;) {
        int f = load_step(&ld);
        if (f & LF_NEW_PAGE) {
            page_free(page);
            page = ld.shown;
            printf("page after %u ms: %s\n", plat_ms() - t0, page->url);
        }
        if (f & LF_DONE || (!(f & ~LF_PAINT) && ld.state == LS_IDLE))
            break;
        if (plat_ms() - t0 > 60000) {
            printf("gave up after 60 s\n");
            break;
        }
        usleep(5000);
    }
    if (!page) {
        printf("no page\n");
        return 1;
    }
    if (getenv("BROWSE_DUMP"))
        dump(page->doc, 0);
    int ok = 0;
    for (int i = 0; i < page->nimgs; i++)
        ok += page->imgs[i].state == IMG_OK;
    int h = page->content_h < 40 ? 40 : page->content_h > MAX_H ? MAX_H : page->content_h;
    gfx_surface s;
    gfx_surface_init(&s, px, 240, h, 240);
    paint_page(page, &s, gfx_r(0, 0, 240, h), 0, gfx_r(0, 0, 240, h), NULL, NULL);
    host_write_bmp(out, &s);
    printf("\"%s\": %d items, %d px tall, %d/%d images, %u KiB arena, %u KiB images, %u ms%s\n",
           page->title ? page->title : "", page->nitems, page->content_h, ok, page->nimgs,
           (unsigned)(page->arena.used / 1024), page->img_bytes / 1024, plat_ms() - t0,
           page->oom ? " (out of memory)" : "");
    load_forget(&ld, page);
    page_free(page);
    script_shutdown();
    return 0;
}
