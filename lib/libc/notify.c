/*
 * Badges and notifications for an app named by path, from processes other
 * than the front app (see glaze/sb.h and glaze/notify.h).
 */

#include <glaze/notify.h>
#include <glaze/sb.h>
#include <glaze/sys.h>
#include <glaze/thread.h>
#include <string.h>

static int port;
static struct sb_note_req *req;

/* One call at a time per process: the request page is the process's. */
static struct gz_mutex req_lock = GZ_MUTEX_INIT;

static void release(struct gz_mutex **m)
{
    gz_mutex_unlock(*m);
}

#define REQ_LOCKED() \
    struct gz_mutex *held_ __attribute__((cleanup(release))) = (gz_mutex_lock(&req_lock), &req_lock)

static int request(uint32_t op, uint32_t a, uint32_t b, uint32_t c)
{
    struct gz_msg m, r;

    memset(&m, 0, sizeof(m));
    m.w[0] = op;
    m.w[1] = a;
    m.w[2] = b;
    m.w[3] = (uint32_t)gz_getpid();
    m.w[4] = c;
    if (gz_send(port, &m, &r) < 0)
        return GZ_EIO;
    return (int)r.w[0];
}

/* SpringBoard may not be running (ktest), so this does not wait for it. */
static int open_client(const char *path)
{
    if (!req) {
        if ((port = gz_port_lookup(SB_PORT_NAME)) <= 0)
            return GZ_ENOENT;
        struct gz_msg m, r;
        memset(&m, 0, sizeof(m));
        m.w[0] = SB_CLIENT_OPEN;
        m.w[3] = (uint32_t)gz_getpid();
        if (gz_send(port, &m, &r) < 0 || (int)r.w[0] < 0)
            return r.w[0] ? (int)r.w[0] : GZ_EIO;
        if (!(req = gz_shm_map((int)r.w[1])))
            return GZ_ENOMEM;
    }
    memset(req, 0, sizeof(*req));
    if (strlcpy(req->path, path, sizeof(req->path)) >= sizeof(req->path))
        return GZ_EINVAL;
    return 0;
}

int sb_badge_set_for(const char *path, int count)
{
    REQ_LOCKED();
    int rv = open_client(path);

    return rv ? rv : request(SB_BADGE_SET_FOR, count < 0 ? 0 : (uint32_t)count, 0, 0);
}

int sb_note_post_for(const char *path, uint32_t id, uint32_t when, const char *title, const char *body,
                     int badge)
{
    REQ_LOCKED();
    int rv = open_client(path);

    if (rv)
        return rv;
    strlcpy(req->title, title ? title : "", sizeof(req->title));
    strlcpy(req->body, body ? body : "", sizeof(req->body));
    return request(SB_NOTE_POST_FOR, when, id, badge < 0 ? 0 : (uint32_t)badge);
}

int sb_note_cancel_for(const char *path, uint32_t id)
{
    REQ_LOCKED();
    int rv = open_client(path);

    return rv ? rv : request(SB_NOTE_CANCEL_FOR, id, 0, 0);
}

_Static_assert(sizeof(struct sb_wince_req)<=sizeof(struct sb_note_req), "CE launch must fit the client page");
int sb_wince_launch(const char *path)
{
    REQ_LOCKED();
    int rv=open_client("");if(rv)return rv;
    struct sb_wince_req *ce=(void *)req;
    if(!path || strlcpy(ce->path,path,sizeof ce->path)>=sizeof ce->path)return GZ_EINVAL;
    return request(SB_WINCE_LAUNCH,0,0,0);
}
