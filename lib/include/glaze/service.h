/* Load-on-demand system services: read-only discovery and privileged policy. */
#ifndef GLAZE_SERVICE_H
#define GLAZE_SERVICE_H
#include <glaze/abi.h>
#define GZ_SERVICE_MAX 8u
#define GZ_SERVICE_NAME 16u
#define GZ_SERVICE_PATH 48u
#define GZ_SERVICE_NETWORK 1u
#define GZ_SERVICE_REQUIRED 2u
#define GZ_SERVICES_MAGIC 0x475a5356u
#define GZ_SERVICES_ABI 1u
struct gz_service_spec {
    char name[GZ_SERVICE_NAME], path[GZ_SERVICE_PATH];
    uint32_t privileges, policy, after;
};
struct gz_service_manifest {
    uint32_t magic, abi, count;
    struct gz_service_spec entries[GZ_SERVICE_MAX];
};
enum { GZ_SERVICE_WAITING, GZ_SERVICE_RUNNING, GZ_SERVICE_DISABLED, GZ_SERVICE_FAILED };
struct gz_service_info {
    char name[GZ_SERVICE_NAME];
    uint32_t state, pid, starts;
    int32_t error;
};
struct gz_service_snapshot {
    uint32_t count, network_kind, driver_pages, generation;
    struct gz_service_info entries[GZ_SERVICE_MAX];
};
#endif
