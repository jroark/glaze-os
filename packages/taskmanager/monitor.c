#include "monitor.h"
#include <string.h>

static unsigned percent(uint64_t part, uint64_t total)
{
    if (!total) return 0;
    if (part >= total) return 100;
    return (unsigned)(part * 100u / total);
}

int monitor_find(const struct monitor *m, uint32_t pid)
{
    for (unsigned i = 0; i < m->now.count; i++)
        if (m->now.tasks[i].pid == pid) return (int)i;
    return -1;
}

bool monitor_can_stop(const struct gz_task_info *t)
{
    return (t->flags & (GZ_TASK_APP | GZ_TASK_SELF)) == GZ_TASK_APP;
}

void monitor_update(struct monitor *m, const struct gz_task_snapshot *s, bool available)
{
    uint64_t elapsed = s->time_us > m->now.time_us ? s->time_us - m->now.time_us : 0;
    m->rate_valid = available && m->available && elapsed && s->busy_us >= m->now.busy_us;
    m->cpu = m->rate_valid ? percent(s->busy_us - m->now.busy_us, elapsed) : 0;
    unsigned count = MIN(s->count, GZ_TASK_MAX);
    for (unsigned i = 0; i < count; i++) {
        const struct gz_task_info *t = &s->tasks[i];
        int old = monitor_find(m, t->pid);
        m->task_cpu[i] = 0;
        if (m->rate_valid && old >= 0 && t->cpu_us >= m->now.tasks[old].cpu_us)
            m->task_cpu[i] = percent(t->cpu_us - m->now.tasks[old].cpu_us, elapsed);
        m->order[i] = i;
    }
    m->now = *s;
    m->now.count = count;
    m->available = available;
    /* Sort by interval CPU, then PID for stable ties. Selection uses PID. */
    for (unsigned i = 1; i < count; i++) {
        unsigned v = m->order[i], j = i;
        while (j && (m->task_cpu[v] > m->task_cpu[m->order[j - 1]] ||
               (m->task_cpu[v] == m->task_cpu[m->order[j - 1]] &&
                s->tasks[v].pid < s->tasks[m->order[j - 1]].pid))) {
            m->order[j] = m->order[j - 1];
            j--;
        }
        m->order[j] = v;
    }
    /* 255 denotes a missing CPU sample (first sample or old kernel). */
    memmove(m->cpu_history, m->cpu_history + 1, MON_HISTORY - 1);
    memmove(m->mem_history, m->mem_history + 1, MON_HISTORY - 1);
    m->cpu_history[MON_HISTORY - 1] = m->rate_valid ? m->cpu : 255;
    m->mem_history[MON_HISTORY - 1] = percent(s->total_pages - MIN(s->free_pages, s->total_pages), s->total_pages);
    if (m->history_count < MON_HISTORY) m->history_count++;
}
