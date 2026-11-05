/* CF data modem: cfuart.ko lifecycle and the network server's raw UART.
 * The socket dispatcher (cf.c) matches the CIS; dialing and PPP run in
 * servers/net. Hayes commands follow the Rockwell RC56 AT command set;
 * docs/hardware/MODEM.md has the verified card.
 */
#include "kernel.h"
#include "drivers.h"
#include "module.h"
#include "uart_card.h"
#include "mips.h"
#include "hw.h"
#include <glaze/modem.h>
static struct module image;
static const struct uart_driver *driver;
static struct uart_config config;
static bool claimed;
static uint32_t generation;
static int bound_port;      /* told GZ_NOTIFY_NET when bytes arrive */
/* 115200, 57600 and 9600 baud from the 16550's usual 1.8432 MHz clock. The
 * modem autobauds on "AT", so the first divisor it answers at is kept. */
static const uint16_t divisors[] = { 1, 2, 12 };

uint32_t modem_generation(void) { return generation; }
bool modem_present(void) { return driver && PCMCIA_CARD_PRESENT(); }
bool modem_ready(void) { return driver && PCMCIA_CARD_PRESENT() && !driver->error(); }
void modem_poll(void)
{
    if (!driver || !PCMCIA_CARD_PRESENT()) return;
    driver->poll();
    uint8_t peek;
    if (bound_port > 0 && driver->read(&peek, 1) > 0)
        ipc_notify(bound_port, GZ_NOTIFY_NET);
}

void modem_removed(void)
{
    if (!claimed) return;
    if (driver) driver->stop();
    driver = NULL; claimed = false; generation++; bound_port = 0;
    module_unload(&image);
}

/* Sends a command and collects the reply for up to ms, stopping at a final
 * result. Callbacks run with interrupts off, as from the interrupt handler. */
static unsigned chat(const struct uart_driver *d, const char *cmd, char *out, unsigned size,
                     uint32_t ms)
{
    unsigned n = 0;
    uint32_t s = irq_save();
    int rv = d->write((const uint8_t *)cmd, strlen(cmd));
    irq_restore(s);
    if (rv < 0) return 0;
    for (uint64_t t0 = time_us(); time_us() - t0 < ms * 1000u && n + 1 < size;) {
        s = irq_save();
        d->poll();
        int got = d->read((uint8_t *)out + n, size - 1 - n);
        if (got > 0) d->consume((uint32_t)got);
        irq_restore(s);
        if (got < 0) break;
        n += got > 0 ? (unsigned)got : 0;
        out[n] = 0;
        if (strstr(out, "OK\r") || strstr(out, "ERROR\r")) break;
    }
    out[n] = 0;
    for (unsigned i = 0; i < n; i++)
        if (out[i] == '\r' || out[i] == '\n') out[i] = ' ';
        else if (out[i] < 32 || out[i] > 126) out[i] = '.';
    return n;
}

void modem_insert(uint32_t cor_address, unsigned config_index)
{
    if (claimed) return;
    claimed = true; generation++;
    const struct uart_driver *d = uart_card_load(&image);
    if (!d) goto failed;
    /* 8N1, FIFO on with a one-byte trigger, DTR, RTS and OUT2 (the card's
     * interrupt enable on PC serial ports). COR: level interrupts. */
    config = (struct uart_config){
        .cor_address = cor_address, .io_base = 0x3f8, .divisor = divisors[0],
        .cor = (uint8_t)(0x40u | (config_index & 0x3Fu)), .lcr = 0x03, .mcr = 0x0b, .fcr = 7,
    };
    char reply[96];
    for (unsigned i = 0; i < sizeof(divisors) / sizeof(divisors[0]); i++) {
        config.divisor = divisors[i];
        if (d->init(&config)) { d->stop(); goto failed; }
        chat(d, "AT\r", reply, sizeof(reply), 400);
        kprintf("modem: COR %x at %x, divisor %u: AT -> \"%s\"\n",
                config.cor, cor_address, config.divisor, reply);
        if (strstr(reply, "OK")) {
            chat(d, "ATI3\r", reply, sizeof(reply), 600);
            kprintf("modem: ATI3 -> \"%s\"\n", reply);
            driver = d;
            return;
        }
    }
    /* No answer at any rate: keep the card, at 115200, for the dialer to retry. */
    config.divisor = divisors[0];
    if (d->init(&config)) { d->stop(); goto failed; }
    driver = d; kputs("modem: UART up, no AT reply\n"); return;
failed:
    module_unload(&image); kputs("modem: UART module unavailable; socket reserved\n");
}

int sys_modem_io(struct proc *p, uint32_t op, uint32_t buffer, uint32_t size)
{
    if (!(p->flags & PROC_NET)) return GZ_EPERM;
    if (p->modem_generation != generation) return GZ_EDEAD;
    if (!driver || !PCMCIA_CARD_PRESENT()) return GZ_ENOENT;
    switch (op) {
    case MODEM_IO_RESET:
        if (size) return GZ_EINVAL;
        driver->stop(); return driver->init(&config);
    case MODEM_IO_LINES:
        if (size & ~(MODEM_DTR | MODEM_RTS)) return GZ_EINVAL;
        driver->set_lines((uint8_t)size); return 0;
    case MODEM_IO_BIND:
        bound_port = (int)size;
        modem_poll();
        return 0;
    case MODEM_IO_STATUS:
        if (size) return GZ_EINVAL;
        if (driver->error()) return driver->error();
        return driver->status();
    }
    if (!size || size > MODEM_IO_MAX) return GZ_EINVAL;
    uint8_t bytes[MODEM_IO_MAX];
    modem_poll();
    if (op == MODEM_IO_WRITE) {
        if (copy_from_as(&p->as, bytes, buffer, size)) return GZ_EINVAL;
        return driver->write(bytes, size);
    }
    if (op == MODEM_IO_READ) {
        int n = driver->read(bytes, size);
        if (n > 0) {
            if (copy_to_as(&p->as, buffer, bytes, (uint32_t)n)) return GZ_EINVAL;
            driver->consume((uint32_t)n);
        }
        return n;
    }
    return GZ_EINVAL;
}
