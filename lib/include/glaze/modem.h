/* CF data modem UART, raw bytes for the network server's dialer and PPP. */
#ifndef GLAZE_MODEM_H
#define GLAZE_MODEM_H
/* gz_modem_io(op, buffer, size), PROC_NET only, at most MODEM_IO_MAX bytes.
 * READ/WRITE return a byte count (WRITE: GZ_EBUSY when the ring is full);
 * LINES takes MODEM_DTR/MODEM_RTS in size; STATUS returns MODEM_MSR_* bits;
 * RESET reprograms the UART and drops anything buffered; BIND takes a port
 * (in size) to be sent GZ_NOTIFY_NET whenever received bytes are waiting. GZ_ENOENT: no
 * modem; GZ_EDEAD: the card this process started with was removed. */
enum { MODEM_IO_RESET, MODEM_IO_READ, MODEM_IO_WRITE, MODEM_IO_LINES, MODEM_IO_STATUS,
       MODEM_IO_BIND };
#define MODEM_IO_MAX 256u
#define MODEM_DTR 0x01u
#define MODEM_RTS 0x02u
#define MODEM_MSR_CTS 0x10u
#define MODEM_MSR_DSR 0x20u
#define MODEM_MSR_RI  0x40u
#define MODEM_MSR_DCD 0x80u
#endif
