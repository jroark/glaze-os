/* Classic Bluetooth services and read-only CF card evidence. */
#ifndef GLAZE_BLUETOOTH_H
#define GLAZE_BLUETOOTH_H
#include <glaze/abi.h>
#define BT_CIS_BYTES 256u
#define BT_CIS_VERSION 1u
/* SERIAL is a candidate, not proof of a Bluetooth controller. */
/* MODEM: a serial card whose FUNCE names a modem function (glaze/modem.h). */
/* GPS: a serial card with no modem FUNCE, handed to the NMEA listener (glaze/gps.h). */
enum { BT_CARD_ABSENT, BT_CARD_OTHER, BT_CARD_SERIAL, BT_CARD_BLUETOOTH, BT_CARD_MODEM, BT_CARD_GPS };
struct bt_card_info {
    uint32_t version, generation, kind;
    uint32_t manufacturer, product, function, cor;
    int32_t config_index;
    uint32_t cis_length;
    uint8_t cis[BT_CIS_BYTES];
    char identification[96];
};
/* Kernel-granted PROC_BLUETOOTH only. Raw byte I/O is bounded to 256 bytes. */
enum { BT_IO_RESET, BT_IO_READ, BT_IO_WRITE };
#define BT_DEVICE_MAX 16u
#define BT_NAME_MAX 40u
#define BT_PORT_NAME "bluetooth"
enum { BT_ABSENT, BT_STARTING, BT_READY, BT_SCANNING, BT_FAILED };
enum { BT_STATUS = 1, BT_SCAN, BT_CANCEL, BT_DEVICE,
       BT_SERIAL_CONNECT, BT_SERIAL_STATUS, BT_SERIAL_WRITE, BT_SERIAL_READ, BT_SERIAL_CLOSE,
       BT_AUDIO_CONNECT, BT_AUDIO_STATUS, BT_AUDIO_CLOSE, BT_AUDIO_TONE,
       BT_AUDIO_ATTACH, BT_AUDIO_WRITE, BT_AUDIO_FLUSH, BT_AUDIO_DETACH, BT_AUDIO_PLAY, BT_AUDIO_PAUSE };
enum { BT_SERIAL_IDLE, BT_SERIAL_CONNECTING, BT_SERIAL_PAIRING, BT_SERIAL_ENCRYPTING,
       BT_SERIAL_DISCOVERING, BT_SERIAL_OPENING, BT_SERIAL_CONNECTED, BT_SERIAL_CLOSING, BT_SERIAL_FAILED };
struct bt_serial_status {
    uint32_t state, id;
    int32_t error;
    uint32_t sent, received, available, authenticated, encrypted;
    uint8_t peer[6], channel, hci_status;
};
struct bt_serial_handle { int port; uint32_t id; };
int bt_serial_connect_channel(const uint8_t peer[6],const void *pin,unsigned pin_length,unsigned channel,struct bt_serial_handle *);
int bt_serial_connect(const uint8_t peer[6],const void *pin,unsigned pin_length,struct bt_serial_handle *);
int bt_serial_status(struct bt_serial_status *);
int bt_serial_write(struct bt_serial_handle,const void *,unsigned);
int bt_serial_read(struct bt_serial_handle,void *,unsigned);
int bt_serial_disconnect(struct bt_serial_handle);
/* Close the session observed on one service port. A replacement session or
 * restarted service cannot be closed through the captured identifier. */
int bt_serial_disconnect_current(void);
/* One outbound A2DP/SBC source. PCM is S16LE at the negotiated rate/channels.
 * Handles capture a service port and session ID; replacement sessions reject
 * stale handles. Keys remain in RAM. Audio and serial share one radio link. */
enum { BT_AUDIO_IDLE, BT_AUDIO_CONNECTING, BT_AUDIO_NEGOTIATING,
       BT_AUDIO_STREAMING, BT_AUDIO_CLOSING, BT_AUDIO_FAILED };
#define BT_AUDIO_BUF_SIZE 8192u
#define BT_AUDIO_RING_SIZE 32768u
struct bt_audio_status {
    uint32_t state,id;
    int32_t error;
    uint32_t rate,channels,bitpool,packets,frames_played,queued,underruns;
    uint32_t authenticated,encrypted;
    uint8_t peer[6],hci_status,reserved;
};
struct bt_audio_stream { int port,shm; uint32_t id,token; void *buffer; };
int bt_audio_connect(const uint8_t peer[6],const void *pin,unsigned length,struct bt_serial_handle *);
int bt_audio_status(struct bt_audio_status *);
int bt_audio_disconnect_current(void);
int bt_audio_tone(unsigned milliseconds);
int bt_audio_stream_open(struct bt_audio_stream *);
int bt_audio_stream_write(struct bt_audio_stream *,const void *,unsigned);
int bt_audio_stream_flush(struct bt_audio_stream *);
int bt_audio_stream_play(struct bt_audio_stream *,bool);
void bt_audio_stream_close(struct bt_audio_stream *);
struct bt_device {
    uint8_t address[6], device_class[3], repetition;
    uint16_t clock_offset;
    char name[BT_NAME_MAX];
};
struct bt_status {
    uint32_t state, generation, revision, count;
    int32_t error;
    uint8_t address[6], hci_version, lmp_version;
    uint16_t manufacturer, hci_revision, lmp_subversion, reserved;
    uint8_t features[8];
    struct bt_device devices[BT_DEVICE_MAX];
};
/* Asynchronous scan/cancel. Status may return GZ_EBUSY if results changed
 * during snapshot collection; retry on the next UI tick. Addresses use the
 * HCI byte order (least significant octet first), names are terminated. */
int bt_status(struct bt_status *);
int bt_scan(void);
int bt_cancel(void);
#endif
