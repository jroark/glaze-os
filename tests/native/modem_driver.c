/* kernel/modem.c: the AT probe at insertion, PROC_NET gating, stale
 * generations, line control, notification and removal. */
#define GLAZE_HW_H
static int socket_present=1;
#define PCMCIA_CARD_PRESENT() (socket_present != 0)
#define GLAZE_MIPS_H
static unsigned irq_off;
static inline unsigned irq_save(void) { return irq_off++; }
static inline void irq_restore(unsigned s) { irq_off = s; }
#include "../../kernel/modem.c"
#include <assert.h>
#include <stdio.h>
static const struct uart_config *configured;
static unsigned inits, stops, unloaded, notified, consumed;
static uint8_t lines, msr_value = 0x30;
static int load_error, init_error, answer_at = 1;    /* divisor that answers AT */
static char reply[64]; static unsigned reply_len, reply_pos;
static uint8_t sent[256]; static unsigned sent_len;
static int initialize(const struct uart_config *c) { configured = c; inits++; reply_len = reply_pos = 0; return init_error; }
static void poll_uart(void) {}
static void stop_uart(void) { stops++; }
static int error_uart(void) { return 0; }
static int read_uart(uint8_t *p, uint32_t n)
{
    unsigned left = reply_len - reply_pos; if (n > left) n = left;
    memcpy(p, reply + reply_pos, n); return (int)n;
}
static void consume_uart(uint32_t n) { reply_pos += n; consumed += n; }
static int write_uart(const uint8_t *p, uint32_t n)
{
    memcpy(sent, p, n); sent_len = n;
    /* The RC56LD echoes, then answers, at the divisor it was configured for. */
    if (configured->divisor == answer_at) {
        if (!memcmp(p, "AT\r", 3)) strcpy(reply, "AT\r\r\nOK\r\n");
        else if (!memcmp(p, "ATI3\r", 5)) strcpy(reply, "ATI3\r\r\nL27-01\r\n\r\nOK\r\n");
        else strcpy(reply, "x");
        reply_len = strlen(reply); reply_pos = 0;
    }
    return (int)n;
}
static void set_lines(uint8_t m) { lines = m; }
static uint8_t msr(void) { return msr_value; }
static struct uart_driver descriptor = { UART_DRIVER_MAGIC, UART_DRIVER_ABI, sizeof(struct uart_driver),
    initialize, poll_uart, stop_uart, error_uart, read_uart, consume_uart, write_uart, set_lines, msr };
int module_load(const char *path, const char *symbol, struct module *m)
{
    assert(!strcmp(path, "drivers/cfuart.ko") && !strcmp(symbol, "glaze_driver"));
    if (load_error) return load_error;
    m->entry = &descriptor; return 0;
}
void module_unload(struct module *m) { if (m->entry) unloaded++; memset(m, 0, sizeof(*m)); }
bool module_contains(const struct module *m, const void *p, uint32_t size) { return p == &descriptor; }
bool module_executable(const struct module *m, const void *p) { return p != NULL; }
void kputs(const char *s) {}
void kprintf(const char *f, ...) {}
static uint64_t clock_us;
uint64_t time_us(void) { return clock_us += 1000; }
int ipc_notify(int port, uint32_t bits) { assert(port == 7 && bits == GZ_NOTIFY_NET); notified++; return 0; }
int copy_from_as(struct addrspace *as, void *p, uint32_t source, uint32_t size) { memset(p, 'A', size); return 0; }
int copy_to_as(struct addrspace *as, uint32_t target, const void *p, uint32_t size) { return 0; }
int main(void)
{
    struct proc net = { .flags = PROC_NET }, app = {0};
    assert(sys_modem_io(&app, MODEM_IO_STATUS, 0, 0) == GZ_EPERM);
    /* Verified card: COR 0x420, entry 5; the first divisor answers. */
    modem_insert(0x420, 5);
    assert(modem_present() && modem_ready() && inits == 1);
    assert(configured->cor_address == 0x420 && configured->cor == 0x45 && configured->io_base == 0x3f8);
    assert(configured->divisor == 1 && configured->lcr == 0x03 && (configured->mcr & 3) == 3);
    assert(!memcmp(sent, "ATI3\r", 5) && reply_pos == reply_len && !irq_off);
    net.modem_generation = modem_generation();
    assert(sys_modem_io(&net, MODEM_IO_STATUS, 0, 0) == 0x30);
    assert(sys_modem_io(&net, MODEM_IO_STATUS, 0, 1) == GZ_EINVAL);
    assert(!sys_modem_io(&net, MODEM_IO_LINES, 0, MODEM_RTS) && lines == MODEM_RTS);
    assert(sys_modem_io(&net, MODEM_IO_LINES, 0, 4) == GZ_EINVAL);
    assert(sys_modem_io(&net, MODEM_IO_WRITE, 1, MODEM_IO_MAX + 1) == GZ_EINVAL);
    assert(sys_modem_io(&net, MODEM_IO_WRITE, 1, 4) == 4 && !memcmp(sent, "AAAA", 4));
    assert(sys_modem_io(&net, 99, 1, 4) == GZ_EINVAL);
    /* Bound: received bytes notify the net server. */
    strcpy(reply, "~}"); reply_len = 2; reply_pos = 0;
    assert(!sys_modem_io(&net, MODEM_IO_BIND, 0, 7) && notified == 1);
    assert(sys_modem_io(&net, MODEM_IO_READ, 1, 16) == 2 && reply_pos == 2);
    unsigned before = notified;
    modem_poll(); assert(notified == before);       /* nothing left to read */
    assert(!sys_modem_io(&net, MODEM_IO_RESET, 0, 0) && inits == 2);
    /* Removal: old handles are dead, the module is released. */
    socket_present = 0; assert(!modem_present() && sys_modem_io(&net, MODEM_IO_READ, 1, 4) == GZ_ENOENT);
    modem_removed(); assert(unloaded == 1 && sys_modem_io(&net, MODEM_IO_READ, 1, 4) == GZ_EDEAD);
    /* A modem that answers only at 9600 (divisor 12) is kept at that rate. */
    socket_present = 1; answer_at = 12; modem_insert(0x420, 5);
    assert(modem_ready() && configured->divisor == 12);
    modem_removed();
    /* No answer at all: kept at 115200 for the dialer to retry. */
    answer_at = 0; modem_insert(0x420, 5);
    assert(modem_ready() && configured->divisor == 1);
    modem_removed();
    /* Module failures leave the socket reserved without a driver. */
    load_error = GZ_ENOENT; modem_insert(0x420, 5); assert(!modem_present()); modem_removed();
    load_error = 0; init_error = GZ_EIO; modem_insert(0x420, 5); assert(!modem_present() && stops); modem_removed();
    puts("modem driver: AT probe and divisor choice, privilege, lines, notification, stale generation, removal PASS");
}
