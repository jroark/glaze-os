#ifndef TASK_MONITOR_H
#define TASK_MONITOR_H
#include <glaze/abi.h>

#define MON_HISTORY 60
struct monitor {
    struct gz_task_snapshot now;
    bool available, rate_valid;
    unsigned cpu, task_cpu[GZ_TASK_MAX];
    unsigned order[GZ_TASK_MAX];
    unsigned history_count;
    uint8_t cpu_history[MON_HISTORY], mem_history[MON_HISTORY];
};
void monitor_update(struct monitor *m, const struct gz_task_snapshot *s, bool available);
int monitor_find(const struct monitor *m, uint32_t pid);
bool monitor_can_stop(const struct gz_task_info *t);
#endif
