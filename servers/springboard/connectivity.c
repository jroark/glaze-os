#include "sb_internal.h"
#include <glaze/bluetooth.h>
#include <glaze/fs.h>
#include <glaze/net.h>
#include <glaze/sys.h>

/* Each transport has its own presence and link state; USB networking can
 * coexist with either a storage card or a Bluetooth card. */
bool sb_update_connectivity(struct sb_view *v)
{
    struct fs_stat st;
    struct net_status net;

    /* A network card can be inserted or pulled too (cf.c's pcmcia_poll);
     * net_status is answered at once, without this process's session. */
    bool mounted = fs_stat("/Card", &st) == 0;
    bool have_net = net_status(&net) == 0 && net.state != NET_NO_CARD;
    unsigned kind = have_net ? net.kind : GZ_NET_NONE;
    bool connected = have_net && (net.state == NET_UP || net.state == NET_UP_STATIC);
    struct bt_card_info card;
    struct bt_serial_status serial;
    bool bluetooth = !gz_bt_card_info(&card, sizeof(card)) && card.kind == BT_CARD_BLUETOOTH;
    bool paired = bluetooth && !bt_serial_status(&serial) && serial.state == BT_SERIAL_CONNECTED;
    bool changed = mounted != v->card_mounted || kind != v->network_kind ||
                   connected != v->network_connected || bluetooth != v->bluetooth_present || paired != v->bluetooth_connected;
    v->card_mounted = mounted;
    v->network_kind = kind; v->network_connected = connected;
    v->bluetooth_present = bluetooth; v->bluetooth_connected = paired;
    return changed;
}
