/* Peer for ktest: echoes messages and serves shm/notify/fault requests. */

#include <glaze/sys.h>
#include <stdio.h>

int main(void)
{
    int port = gz_port_create();
    gz_port_register(port, "pong");

    for (;;) {
        struct gz_msg m;
        int tok = gz_recv(port, &m, -1);
        if (tok <= 0)
            continue;
        if (m.w[0] == 0xFFFFFFFF) {
            uint32_t *p = gz_shm_map((int)m.w[1]);
            if (p) {
                p[0] = 0xC0FFEE;
                p[16383] = 0xBEEF;
            }
        } else if (m.w[0] == 0xFFFFFFFE) {
            gz_notify(gz_port_lookup("ktest"), 0x55);
        } else if (m.w[0] == 0xFFFFFFFD) {
            volatile uint32_t *bad = (uint32_t *)0;
            *bad = 1;
        } else {
            m.w[0]++;
        }
        gz_reply(tok, &m);
    }
}
