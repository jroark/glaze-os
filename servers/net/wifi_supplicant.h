#ifndef GLAZE_WIFI_SUPPLICANT_H
#define GLAZE_WIFI_SUPPLICANT_H
#include <glaze/wifi.h>
struct wifi_supplicant_io {
    int (*send)(const uint8_t *, unsigned);
    int (*key)(const struct wifi_key *);
    int (*authorize)(void);
    void (*failed)(int);
    int (*random)(uint8_t *, unsigned);
    uint32_t (*time_ms)(void);
};
int wifi_supplicant_prepare(const struct wifi_supplicant_io *, const struct wifi_credentials *,
                            const struct wifi_ap *, const uint8_t mac[6], struct wifi_config *);
void wifi_supplicant_associated(void);
void wifi_supplicant_rx(const uint8_t *frame, unsigned length);
void wifi_supplicant_poll(void);
void wifi_supplicant_stop(void);
int wifi_supplicant_state(void);
#endif
