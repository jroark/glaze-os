/* BE-300 companion SIU: hardware.txt line 191, be300 documentation
 * EMULATED_HARDWARE_LINUX_PORTING.md, UARTs (stride 4 at PA 0x0A008680).
 * This board-specific UART is not described by the generic VRC4173 UM.
 * Stowaway.dll 0x1036c0 configures 9600 8N1; 0x103dd4 checks FA FD.
 * DTR + RTS rising-edge probe follows Stowaway.dll (see the state machine
 * below). The dock is watched, the handshake run and RX polled on the
 * existing 100 Hz tick, so the keyboard can be docked and undocked at will.
 *
 * Dock port power: Windows CE's serial.dll opens COM1: with
 * ModemSockOpenDevice(1, 0x1F00, 0) (serial.dll 0x01942ca4). socket.dll
 * (XIP 0x01930000) then drives GPIO6 high through cdm.dll WritePortDataEx
 * (0x144, 0x40, 0x40) (0x0193115c; GIUPIODL, VR4131 UM ch. 15, which
 * companion_gates_on already sets) and writes the dock control register
 * 0x0A008000 = (old & ~0xBF1F) | ((type | 0x1F00) & 0xBF1F), where type is
 * the dock socket ID at 0x0A008010 & 0x1F (0x0193118c..0x019311a4; register
 * IDs 4 and 2 of its 0xAA008000 window, table 0x0193112c). Closing writes
 * type | 0x2000 instead, and Windows CE's idle state reads 0x200C there.
 * Without this a NAND boot never powers the dock's transceiver, which
 * also powers the keyboard through DTR/RTS.
 */
#include "../kernel.h"
#include "../hw.h"
#include "../drivers.h"
#include "stowaway.h"
#define U(n) REG8(UART_BASE + (n) * 4u)
#define DOCK_CTRL       REG16(0x0A008000u)
#define DOCK_SOCKET     REG16(0x0A008010u)
#define DOCK_RS232      0x08u           /* socket.dll's serial.dll entry */
static struct stowaway_decoder decoder;
static bool connected;

/* The handshake, one step per tick: S_DCD waits for the keyboard's DCD
 * pulse, S_SETTLE holds RTS low, S_ACK waits for FA FD after the RTS edge. */
enum { S_IDLE, S_DCD, S_SETTLE, S_ACK, S_CONNECTED, S_GAVE_UP };
static unsigned state, dock = ~0u, attempts;
static unsigned seen = ~0u;             /* the dock ID being debounced */
static uint32_t seen_at;
static uint32_t phase_at, probe_start;
static bool dcd_seen;
static unsigned ack;
static uint8_t msr;
static struct { uint8_t lcr, mcr, ier, dll, dlm; bool valid; } saved;

#ifdef GLAZE_STOWAWAY_TRACE
/* STOWAWAY_TRACE=1 bring-up log, read back with `glaze-usb logs` since the
 * dock takes the USB and serial connector. The probe runs on the console's
 * own UART at 9600 baud, so its events are kept here and printed after it. */
static struct { uint16_t ms; uint8_t what, val; } probe_log[64];
static unsigned probe_n, probe_lost;
static uint16_t dock_before, dock_after;
static uint8_t msr0, lsr0;
static void probe_note(uint32_t ms, uint8_t what, uint8_t val)
{
    if (probe_n < sizeof(probe_log) / sizeof(probe_log[0]))
        probe_log[probe_n++] = (typeof(probe_log[0])){ (uint16_t)ms, what, val };
    else
        probe_lost++;
}
static void probe_print(void)
{
    kprintf("stowaway: dock control %04x -> %04x\n", dock_before, dock_after);
    kprintf("stowaway: before LCR %02x MCR %02x IER %02x MSR %02x LSR %02x console %s\n",
            saved.lcr, saved.mcr, saved.ier, msr0, lsr0, console_dead() ? "dead" : "live");
    for (unsigned i = 0; i < probe_n; i++)
        kprintf("stowaway: %4u ms %s %02x\n", probe_log[i].ms,
                probe_log[i].what == 'd' ? "MSR at DTR   " :
                probe_log[i].what == 'r' ? "MSR after RTS" :
                probe_log[i].what == 'm' ? "MSR change   " :
                probe_log[i].what == 'e' ? "LSR error    " : "rx           ",
                probe_log[i].val);
    if (probe_lost)
        kprintf("stowaway: %u probe events not logged\n", probe_lost);
    probe_n = probe_lost = 0;
}
#define TRACE(...) __VA_ARGS__
#else
#define TRACE(...)
#endif

int stowaway_status(void) { return connected ? GZ_INPUT_STOWAWAY : 0; }
static void emit(uint16_t type, uint16_t code) { input_push(type, code, 0, 0); }
static uint32_t now_ms(void) { return (uint32_t)(time_us() / 1000u); }

/* Power the dock's serial port as socket.dll does for an RS-232 dock (see
 * the header). */
static void dock_power_on(void)
{
    TRACE(dock_before = DOCK_CTRL;)
    DOCK_CTRL = (uint16_t)((DOCK_CTRL & ~0xbf1fu) | ((DOCK_RS232 | 0x1f00u) & 0xbf1fu));
    TRACE(dock_after = DOCK_CTRL;)
}

/* Stowaway.dll's handshake: its port open (0x0010380c) raises DTR after
 * SetCommMask(EV_RLSD) (0x001037e4), so it waits for a DCD change first.
 * Then, once CTS shows a keyboard (GetCommModemStatus MS_CTS_ON,
 * 0x00103b98), it clears RTS (0x00103cc0), sleeps 500 ms (0x00103d5c),
 * raises RTS (0x00103d70) and reads FA FD (0x00103dcc). A real Targus
 * raises DCD about 550 ms after DTR and drops it about 200 ms later; an
 * RTS edge before that goes unanswered. */
#define PROBE_DCD_MS        1500    /* longest wait for the DCD pulse to end */
#define PROBE_SETTLE_MS     500
#define PROBE_ACK_MS        1000
#define PROBE_ATTEMPTS      3       /* per docking: a plain sync cable is also 08 */
#define PROBE_RETRY_MS      2000
#define MSR_DCD             0x80u
/* While the connector slides in the ID passes through others (a real
 * BE-300 read 0f, 0e, 0a, 08 on one docking; 0f is nothing docked). */
#define DOCK_STABLE_MS      100

static void probe_start_now(uint32_t now)
{
    if (!saved.valid) {
        saved.lcr = U(3);
        U(3) = saved.lcr & ~0x80u;
        saved.mcr = U(4);
        saved.ier = U(1);
        TRACE(msr0 = U(6); lsr0 = U(5);)
        U(3) = 0x83;
        saved.dll = U(0); saved.dlm = U(1);
        saved.valid = true;
    }
    /* The console moves off the companion SIU while the dock may own it. */
    console_use_debug_uart(true);
    dock_power_on();
    U(1) = 0;                       /* polled */
    U(3) = 0x83;                    /* DLAB, 8N1 */
    U(0) = 120; U(1) = 0;           /* linux4be serial_be300.c baud_table:
                                     * SIU baud clock 1.152 MHz / 120 = 9600 */
    U(3) = 3;
    /* 16550 FCR bit 0 (be300/gxemul thirdparty/comreg.h): FA FD and key
     * bursts arrive between 10 ms polls. */
    U(2) = 1;
    for (int n = 0; n < 128 && (U(5) & 1); n++)
        (void)U(0);
    U(4) = 1;                       /* DTR, RTS low */
    msr = U(6);
    dcd_seen = (msr & MSR_DCD) != 0;
    TRACE(probe_note(0, 'd', msr);)
    ack = 0;
    probe_start = phase_at = now;
    state = S_DCD;
    attempts++;
}

/* Put the UART back as it was found: the sync cable's console. */
static void port_release(void)
{
    if (saved.valid) {
        U(4) = saved.mcr;
        U(3) = 0x83;
        U(0) = saved.dll; U(1) = saved.dlm;
        U(3) = saved.lcr & ~0x80u;
        U(1) = saved.ier;
        U(3) = saved.lcr;
        saved.valid = false;
    }
    console_use_debug_uart(false);
}

/* Drop the keyboard: release whatever it held so nothing stays pressed. */
static void disconnect(void)
{
    for (unsigned k = 0; k < sizeof(decoder.held) / sizeof(decoder.held[0]); k++)
        if (decoder.held[k])
            emit(GZ_EV_KEY_UP, decoder.held[k]);
    memset(&decoder, 0, sizeof(decoder));
    connected = false;
}

static void probe_step(uint32_t now)
{
    uint32_t ms = now - probe_start;
    uint8_t m = U(6);
    TRACE(if ((m & 0xf0) != (msr & 0xf0)) probe_note(ms, 'm', m);)
    if (m & MSR_DCD)
        dcd_seen = true;
    msr = m;
    if (state == S_DCD && ((dcd_seen && !(m & MSR_DCD)) || ms >= PROBE_DCD_MS)) {
        state = S_SETTLE; phase_at = now;
    } else if (state == S_SETTLE && now - phase_at >= PROBE_SETTLE_MS) {
        U(4) = 3;                   /* the RTS edge the keyboard answers */
        state = S_ACK; phase_at = now; ack = 0;
        TRACE(probe_note(ms, 'r', U(6));)
    }
    for (int n = 0; n < 16; n++) {
        uint8_t lsr = U(5);
        TRACE(if (lsr & 0x1e) probe_note(ms, 'e', lsr);)
        if (!(lsr & 1))
            break;
        uint8_t byte = U(0);
        TRACE(probe_note(ms, 'x', byte);)
        if (stowaway_ack(&ack, byte) && state == S_ACK) {
            state = S_CONNECTED;
            connected = true;
            attempts = 0;
            kputs("input: Stowaway keyboard connected\n");
            TRACE(probe_print();)
            return;
        }
    }
    if (state == S_ACK && now - phase_at >= PROBE_ACK_MS) {
        U(4) = 1;                   /* RTS low for the next attempt */
        TRACE(probe_print();)
        if (attempts >= PROBE_ATTEMPTS) {
            kputs("input: no Stowaway keyboard on the serial dock\n");
            port_release();
            state = S_GAVE_UP;
        } else {
            state = S_IDLE;         /* retried below after PROBE_RETRY_MS */
            phase_at = now;
        }
    }
}

void stowaway_init(void)
{
    stowaway_poll();                /* the first dock check; the tick does the rest */
}

/* After a sleep the dock lost power: shake hands again. */
void stowaway_resume(void)
{
    if (connected)
        disconnect();
    port_release();
    dock = seen = ~0u;
    state = S_IDLE;
}

void stowaway_poll(void)
{
    uint32_t now = now_ms();
    unsigned type = DOCK_SOCKET & 0x1fu;
    if (type != seen) {
        seen = type;
        seen_at = now;
    }
    if (type != dock && (now - seen_at >= DOCK_STABLE_MS || dock == ~0u)) {
        if (type == DOCK_RS232)
            console_use_debug_uart(true);   /* the line may be the keyboard's */
        kprintf("input: dock socket %02x\n", type);
        if (connected) {
            disconnect();
            kputs("input: Stowaway keyboard disconnected\n");
        }
        TRACE(if (state != S_IDLE && state != S_GAVE_UP) probe_print();)
        if (dock == DOCK_RS232 || saved.valid)
            port_release();
        if (type == DOCK_RS232)
            console_use_debug_uart(true);
        dock = type;
        attempts = 0;
        state = S_IDLE;
        phase_at = now - PROBE_RETRY_MS;    /* probe at once */
    }
    if (dock != DOCK_RS232)
        return;
    switch (state) {
    case S_IDLE:
        if (now - phase_at >= PROBE_RETRY_MS)
            probe_start_now(now);
        return;
    case S_DCD: case S_SETTLE: case S_ACK:
        probe_step(now);
        return;
    case S_GAVE_UP:
        return;
    }
    for (int n = 0; n < 128; n++) {
        uint8_t lsr = U(5);
        if (lsr & 0x1e)             /* parity/framing/break/overrun: reset modifiers */
            memset(&decoder, 0, sizeof(decoder));
        if (!(lsr & 1))
            break;
        uint8_t byte = U(0);
        TRACE(kprintf("stowaway: rx %02x%s\n", byte, (lsr & 0x1e) ? " (error)" : "");)
        if (!(lsr & 0x1e))
            stowaway_decode(&decoder, byte, now, emit);
    }
    stowaway_repeat(&decoder, now, emit);
}
