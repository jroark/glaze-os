/* Keep this package loadable on OS versions predating the radio extension. */
#include "client.h"
#include <glaze/audio.h>
#include <glaze/sys.h>
#include <string.h>

static int port, session = -1;
static void *buffer;
static int connect(void)
{
    if (session >= 0) return 0;
    port = gz_port_lookup(AUDIO_PORT_NAME);
    if (port <= 0) return GZ_ENOENT;
    struct gz_msg m = {{0}}, r = {{0}};
    m.w[0] = AUD_HELLO; m.w[1] = (uint32_t)gz_getpid();
    if (gz_send(port, &m, &r) < 0) return GZ_EIO;
    if ((int)r.w[0] < 0) return (int)r.w[0];
    buffer = gz_shm_map((int)r.w[2]);
    if (!buffer) return GZ_ENOMEM;
    session = (int)r.w[1];
    return 0;
}
static int call(int op)
{
    struct gz_msg m = {{0}}, r = {{0}};
    m.w[0] = (uint32_t)op; m.w[1] = (uint32_t)session;
    return gz_send(port, &m, &r) < 0 ? GZ_EIO : (int)r.w[0];
}
int somafm_state(struct radio_state *state)
{
    int rv = connect();
    if (!rv) rv = call(AUD_RADIO_STATE);
    if (!rv) memcpy(state, buffer, sizeof(*state));
    return rv;
}
int somafm_play(const struct radio_request *request)
{
    int rv = connect();
    if (rv) return rv;
    memcpy(buffer, request, sizeof(*request));
    return call(AUD_RADIO_PLAY);
}
int somafm_stop(void)
{
    int rv = connect();
    return rv ? rv : call(AUD_RADIO_STOP);
}
