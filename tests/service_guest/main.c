/* Deferred service lifecycle, no ROMs or external test endpoints. */
#include <glaze/sys.h>
#include <glaze/net.h>
#include <glaze/wifi.h>
#include <stdio.h>
#include <string.h>
#ifndef SERVICE_HOTPLUG
#define SERVICE_HOTPLUG 0
#endif
#define EVENT_WAIT (SERVICE_HOTPLUG ? 60000u : 600u)
static int failed;
#define CHECK(c, label) do { if (!(c)) { printf("service-test: FAIL %s\n", label); failed = 1; goto done; } } while (0)
static struct gz_service_snapshot snapshot;
static struct gz_service_info *network(void)
{
    if (gz_services(&snapshot)) return NULL;
    for (unsigned i = 0; i < snapshot.count; i++)
        if (!strcmp(snapshot.entries[i].name, "net")) return &snapshot.entries[i];
    return NULL;
}
static int wait_service(int up, int previous)
{
    for (unsigned i = 0; i < 200; i++) {
        struct gz_service_info *n = network();
        if (n && (up ? (n->pid && n->pid != (unsigned)previous) : !n->pid)) return 0;
        gz_sleep_ms(50);
    }
    return -1;
}
int main(void)
{
    struct gz_service_info *n = network();
    struct net_status status;
    CHECK(n && snapshot.count == 3, "catalog");
    CHECK(gz_net_handle() == GZ_EPERM, "network capability denied to compositor");
    CHECK(gz_service_set("fs", 0) == GZ_EPERM, "required service cannot be disabled");
    CHECK(gz_service_set("unknown", 1) == GZ_ENOENT, "unknown service rejected");
    CHECK(!SERVICE_HOTPLUG || !snapshot.network_kind, "hotplug starts with empty socket");
    if (!snapshot.network_kind) {
        uint32_t start = gz_time_ms();
        CHECK(!n->pid && !snapshot.driver_pages, "absent card has no driver or service");
        CHECK(!net_status(&status) && status.state == NET_NO_CARD, "offline status");
        CHECK(net_socket(NET_TCP) == GZ_ENETDOWN && gz_time_ms() - start < 100, "offline calls return immediately");
        printf("service-test: offline PASS\n");
        /* Emulator --net-insert-after may add a card; wait in guest time. */
        for (unsigned i = 0; i < EVENT_WAIT; i++) {
            gz_sleep_ms(50);
            n = network();
            if (snapshot.network_kind) break;
        }
        CHECK(!SERVICE_HOTPLUG || snapshot.network_kind, "expected insertion");
        if (!snapshot.network_kind) goto done;
        CHECK(wait_service(1, 0) == 0 && snapshot.driver_pages, "insertion loads driver and service");
        printf("service-test: inserted PASS\n");
    }
    CHECK(wait_service(1, 0) == 0, "service started");
    n = network();
    int old_pid = n->pid;
    uint32_t pages = snapshot.driver_pages;
    CHECK(pages && snapshot.network_kind, "one driver resident");
    if (snapshot.network_kind != GZ_NET_NE2000) {
        struct wifi_status wifi;
        CHECK(!net_wifi_status(&wifi), "wireless controls after module insertion");
        CHECK(!net_wifi_connect("BE300", 5), "wireless association after module insertion");
    }
    CHECK(!net_wait_up(15000), "DHCP");
    int old_socket = net_socket(NET_UDP);
    CHECK(old_socket >= 0, "create old socket");
    CHECK(!gz_service_set("net", 0) && !wait_service(0, 0), "stop service");
    CHECK(!gz_proc_alive(old_pid) && snapshot.driver_pages == pages, "service memory released, card retained");
    CHECK(net_socket(NET_TCP) == GZ_ENETDOWN, "stopped service unavailable");
    CHECK(!gz_service_set("net", 1) && !wait_service(1, old_pid), "restart service");
    if (old_socket >= 0) {
        CHECK(!net_wait_up(15000), "DHCP after restart");
        int fresh = net_socket(NET_UDP);
        if (fresh == GZ_ENETDOWN) fresh = net_socket(NET_UDP);
        CHECK(fresh >= 0 && fresh != old_socket, "socket handles survive restart without aliasing");
        CHECK(net_bind(old_socket, 9001) == GZ_EINVAL, "stale socket rejected");
        CHECK(net_close(fresh) == 0, "new socket works");
    }
    printf("service-test: restart PASS\n");
    /* --cf-eject-after also removes a network card. */
    for (unsigned i = 0; i < EVENT_WAIT; i++) {
        gz_sleep_ms(50);
        n = network();
        if (!snapshot.network_kind) break;
    }
    CHECK(!SERVICE_HOTPLUG || !snapshot.network_kind, "expected removal");
    if (!snapshot.network_kind) {
        CHECK(!wait_service(0, 0) && !snapshot.driver_pages, "removal unloads driver and service");
        CHECK(!net_status(&status) && status.state == NET_NO_CARD, "offline after removal");
        printf("service-test: removed PASS\n");
    }
done:
    if (!failed) printf("service-test: ALL PASS\n");
    for (;;) gz_sleep_ms(1000);
}
