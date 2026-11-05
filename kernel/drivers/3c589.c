/*
 * 3Com EtherLink III PC Card (3c589, 3c589B/C/D), through a PCMCIA jacket.
 *
 * Sixteen I/O ports in register windows: the command/status register at
 * 0x0E is common to all, the rest is chosen by SelectWindow. Window 0 holds
 * configuration and the station-address EEPROM, window 2 the station
 * address filter, window 4 media control, and window 1, the operating set,
 * the transmit and receive FIFOs, which the CPU fills and drains by PIO.
 * Register use follows the 3Com "EtherLink III Parallel Tasking Adapter
 * Technical Reference" (3c509/3c589 register windows and commands) and
 * Linux 3c589_cs.c (tc589_reset, el3_start_xmit, el3_rx, read_eeprom).
 *
 * The card decodes four address lines, so cf.c's network configuration
 * puts it in the same 0x300 window as the NE2000 and XI-825, and picks the
 * PCMCIA bus width whose 16-bit read-back works (configure_network). FIFO
 * data moves 16 bits at a time; frames are padded to a dword in the FIFO.
 */
#ifndef EL3_TEST
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#define EL3_BASE        (pcmcia_io_base + 0x300u)   /* hw.h: the socket holding the card */
#define el3_in8(r)      REG8(EL3_BASE + (r))
#define el3_in16(r)     REG16(EL3_BASE + (r))
#define el3_out8(r, v)  (REG8(EL3_BASE + (r)) = (v))
#define el3_out16(r, v) (REG16(EL3_BASE + (r)) = (v))
#define el3_enable()    (KJ_SRC_PCMCIA |= KJ_SRC_ENABLE)
#define el3_disable()   (KJ_SRC_PCMCIA &= ~KJ_SRC_ENABLE)
#endif

#define EL3_CMD         0x0E            /* write */
#define EL3_STATUS      0x0E            /* read */

/* Commands: the opcode in bits 11-15, an argument below. */
#define CMD(op)         ((uint16_t)((op) << 11))
#define TOTAL_RESET     CMD(0)
#define SELECT_WINDOW   CMD(1)
#define START_COAX      CMD(2)
#define RX_DISABLE      CMD(3)
#define RX_ENABLE       CMD(4)
#define RX_RESET        CMD(5)
#define RX_DISCARD      CMD(8)
#define TX_ENABLE       CMD(9)
#define TX_DISABLE      CMD(10)
#define TX_RESET        CMD(11)
#define ACK_INTR        CMD(13)
#define SET_INTR_ENB    CMD(14)
#define SET_STATUS_ENB  CMD(15)
#define SET_RX_FILTER   CMD(16)
#define SET_TX_THRESH   CMD(18)
#define STATS_DISABLE   CMD(22)
#define STOP_COAX       CMD(23)

/* Status / interrupt bits. */
#define INT_LATCH       0x0001
#define ADAPTER_FAILURE 0x0002
#define TX_COMPLETE     0x0004
#define TX_AVAILABLE    0x0008
#define RX_COMPLETE     0x0010
#define RX_EARLY        0x0020
#define INT_REQ         0x0040
#define STATS_FULL      0x0080
#define CMD_BUSY        0x1000

#define RX_STATION      0x01
#define RX_BROADCAST    0x04

/* Window 0. */
#define W0_MANUFACTURER 0x00            /* 0x6D50, "3Com" */
#define W0_CONFIG_CTRL  0x04
#define W0_ADDR_CONFIG  0x06
#define W0_RESOURCE     0x08
#define W0_EEPROM_CMD   0x0A
#define W0_EEPROM_DATA  0x0C
#define EEPROM_READ     0x0080
#define EEPROM_BUSY     0x8000
/* Window 1. */
#define W1_FIFO         0x00
#define W1_RX_STATUS    0x08
#define W1_TX_STATUS    0x0B
#define W1_TX_FREE      0x0C
#define RX_INCOMPLETE   0x8000
#define RX_ERROR        0x4000
#define RX_LENGTH       0x07FF
#define TXS_COMPLETE    0x80
#define TXS_JABBER      0x20
#define TXS_UNDERRUN    0x10
#define TXS_MAXCOLL     0x08
/* Window 4. */
#define W4_MEDIA        0x0A
#define MEDIA_LED       0x0001
#define MEDIA_TP        0x00C0          /* link beat and jabber guard */

#define FRAME_MAX       1536

static bool present;
static uint8_t mac[6];
static uint8_t frame[FRAME_MAX + 4];

static void window(unsigned w) { el3_out16(EL3_CMD, SELECT_WINDOW | w); }

/* A command that sets CMD_BUSY until it finishes (resets, RX_DISCARD).
 * Linux tc589_wait_for_completion polls the same way. */
static bool command(uint16_t cmd)
{
    el3_out16(EL3_CMD, cmd);
    for (unsigned i = 0; i < 20000; i++) {
        uint16_t st = el3_in16(EL3_STATUS);
        if (st == 0xFFFF) return false;         /* the card is gone */
        if (!(st & CMD_BUSY)) return true;
    }
    return false;
}

/* EEPROM word: an EEPROM read takes 162 us (Linux read_eeprom). */
static uint16_t eeprom(unsigned index)
{
    el3_out16(W0_EEPROM_CMD, EEPROM_READ | index);
    for (unsigned i = 0; i < 20000; i++)
        if (!(el3_in16(W0_EEPROM_CMD) & EEPROM_BUSY))
            break;
    return el3_in16(W0_EEPROM_DATA);
}

/* The FIFO, 16 bits at a time: the bridge passes the card's low byte at
 * the even address (as for the XI-825's data port). */
static void fifo_read(uint8_t *dst, unsigned len)
{
    for (unsigned i = 0; i < len; i += 2) {
        uint16_t w = el3_in16(W1_FIFO);
        dst[i] = (uint8_t)w;
        dst[i + 1] = (uint8_t)(w >> 8);
    }
}

static void fifo_write(const uint8_t *src, unsigned len)
{
    unsigned padded = (len + 3) & ~3u, i = 0;

    for (; i + 1 < len; i += 2)
        el3_out16(W1_FIFO, (uint16_t)(src[i] | src[i + 1] << 8));
    if (i < len) {
        el3_out16(W1_FIFO, src[i]);
        i += 2;
    }
    for (; i < padded; i += 2)
        el3_out16(W1_FIFO, 0);
}

static void receive(void)
{
    for (unsigned n = 0; n < 32; n++) {
        uint16_t rs = el3_in16(W1_RX_STATUS);
        if (rs == 0xFFFF || (rs & RX_INCOMPLETE))
            return;
        unsigned len = rs & RX_LENGTH;
        if ((rs & RX_ERROR) || len < 14 || len > FRAME_MAX) {
            net_rx_drop();
        } else {
            fifo_read(frame, (len + 3) & ~3u);
            net_receive(frame, len);
        }
        if (!command(RX_DISCARD))
            return;
    }
}

/* Pop the transmit status stack; errors other than collisions need the
 * transmitter reset (Linux pop_tx_status). Returns false on an error. */
static bool tx_status(void)
{
    bool ok = true;

    for (unsigned n = 0; n < 8; n++) {
        uint8_t ts = el3_in8(W1_TX_STATUS);
        if (ts == 0xFF || !(ts & TXS_COMPLETE))
            break;
        if (ts & (TXS_JABBER | TXS_UNDERRUN | TXS_MAXCOLL)) {
            ok = false;
            if (ts & (TXS_JABBER | TXS_UNDERRUN))
                command(TX_RESET);
            el3_out16(EL3_CMD, TX_ENABLE);
        }
        el3_out8(W1_TX_STATUS, 0);
    }
    return ok;
}

void el3_irq(void)
{
    if (!present)
        return;
    for (unsigned n = 0; n < 8; n++) {
        uint16_t st = el3_in16(EL3_STATUS);
        if (st == 0xFFFF || !(st & (INT_LATCH | RX_COMPLETE | ADAPTER_FAILURE | TX_COMPLETE)))
            break;
        if (st & RX_COMPLETE)
            receive();
        if (st & TX_COMPLETE)
            tx_status();
        if (st & ADAPTER_FAILURE) {
            /* An RX FIFO overrun: reset and restart the receiver. */
            net_overrun();
            command(RX_RESET);
            el3_out16(EL3_CMD, SET_RX_FILTER | RX_STATION | RX_BROADCAST);
            el3_out16(EL3_CMD, RX_ENABLE);
        }
        el3_out16(EL3_CMD, ACK_INTR | INT_LATCH | ADAPTER_FAILURE | TX_COMPLETE |
                  TX_AVAILABLE | RX_EARLY | INT_REQ);
    }
}

bool el3_init(void)
{
    present = false;
    el3_disable();
    window(0);
    uint16_t id = el3_in16(W0_MANUFACTURER);
    if (id != 0x6D50) {
        kprintf("3c589: window 0 reads %x, not 3Com's 6d50\n", id);
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        uint16_t w = eeprom(i);
        mac[2 * i] = (uint8_t)(w >> 8);
        mac[2 * i + 1] = (uint8_t)w;
    }
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
        kprintf("3c589: bad station address %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }
    command(TX_RESET);
    command(RX_RESET);
    el3_out16(EL3_CMD, SET_INTR_ENB);           /* none while setting up */
    window(0);
    el3_out16(W0_CONFIG_CTRL, 0x0001);          /* enable the adapter */
    el3_out16(W0_RESOURCE, 0x3F00);             /* IRQ field; the socket routes it */
    el3_out16(W0_ADDR_CONFIG, 0);               /* 10BASE-T */
    el3_out16(EL3_CMD, STOP_COAX);
    window(4);
    el3_out16(W4_MEDIA, MEDIA_LED | MEDIA_TP);
    window(2);
    for (unsigned i = 0; i < 6; i++)
        el3_out8(i, mac[i]);
    el3_out16(EL3_CMD, STATS_DISABLE);
    window(1);
    el3_out16(EL3_CMD, SET_RX_FILTER | RX_STATION | RX_BROADCAST);
    el3_out16(EL3_CMD, RX_ENABLE);
    el3_out16(EL3_CMD, TX_ENABLE);
    el3_out16(EL3_CMD, SET_STATUS_ENB | 0xFF);
    el3_out16(EL3_CMD, ACK_INTR | 0xFF);
    el3_out16(EL3_CMD, SET_INTR_ENB | INT_LATCH | RX_COMPLETE | TX_COMPLETE | ADAPTER_FAILURE);
    present = true;
    net_set_mac(mac);
    el3_enable();
    kprintf("3c589: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool el3_present(void) { return present; }

void el3_removed(void)
{
    el3_disable();
    if (present && PCMCIA_CARD_PRESENT()) {
        el3_out16(EL3_CMD, SET_INTR_ENB);
        el3_out16(EL3_CMD, RX_DISABLE);
        el3_out16(EL3_CMD, TX_DISABLE);
    }
    present = false;
}

/* One frame into the transmit FIFO: a length dword, then the padded data.
 * The FIFO holds about two frames; wait for room rather than queue. */
int el3_send(const uint8_t *buf, uint32_t len)
{
    if (!present)
        return GZ_ENOENT;
    if (len > FRAME_MAX)
        return GZ_EINVAL;
    uint32_t wire = len < 60 ? 60 : len;
    uint64_t t0 = time_us();
    while (el3_in16(W1_TX_FREE) < wire + 4) {
        uint16_t free = el3_in16(W1_TX_FREE);
        if (free == 0xFFFF || time_us() - t0 > 20000u)
            return GZ_EIO;
        tx_status();
    }
    el3_out16(W1_FIFO, (uint16_t)wire);
    el3_out16(W1_FIFO, 0);
    if (wire == len) {
        fifo_write(buf, len);
    } else {
        memcpy(frame, buf, len);
        memset(frame + len, 0, wire - len);
        fifo_write(frame, wire);
    }
    return tx_status() ? (int)len : GZ_EIO;
}
