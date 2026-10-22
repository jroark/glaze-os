/* Production sampling, selection, confirmation and rendering with fake counters. */
#define main taskmanager_app_main
#include "../packages/taskmanager/main.c"
#undef main
#include <assert.h>
#include "hostres.h"

struct ui_app ui;
static uint16_t pixels[UI_W * UI_H];
static uint32_t clock_ms;
static int stops, stopped_pid;
static bool old_kernel;
static struct gz_task_snapshot fixture;
uint32_t gz_time_ms(void) { return clock_ms; }
int gz_task_snapshot(struct gz_task_snapshot *s) { *s = fixture; return old_kernel ? GZ_EINVAL : 0; }
int gz_sysinfo(struct gz_sysinfo *s) {
    memset(s, 0, sizeof(*s)); s->total_pages = 4096; s->free_pages = 2048; s->uptime_ms = clock_ms; return 0;
}
int gz_task_stop(int pid) { stops++; stopped_pid = pid; return 0; }
void ui_init(int style) {}
int ui_next_event(struct gz_input_event *e, int timeout) { return 0; }
void ui_invalidate_all(void) {}
void ui_present(void) {}

static void shot(const char *name) {
    char path[128]; draw();
    snprintf(path, sizeof(path), "../out/host/taskmanager-%s.bmp", name);
    host_write_bmp(path, &ui.surf);
}

int main(void)
{
    gfx_surface_init(&ui.surf, pixels, UI_W, UI_H, UI_W);
    ui_list_init(&tasks, gfx_r(0, 96, UI_W, UI_H - 96), 44);
    fixture = (struct gz_task_snapshot){.time_us = 1000000, .busy_us = 100000,
        .total_pages = 4096, .free_pages = 2048, .page_size = 4096, .count = 3,
        .tasks = {{.pid = 33, .threads = 2, .mapped_pages = 123, .cpu_us = 20000, .name = "springboard"},
                  {.pid = 65, .flags = GZ_TASK_APP | GZ_TASK_BG, .threads = 1, .mapped_pages = 100, .cpu_us = 10000, .name = "hello"},
                  {.pid = 97, .flags = GZ_TASK_APP | GZ_TASK_SELF, .threads = 1, .mapped_pages = 155, .cpu_us = 30000, .name = "taskmanager"}}};
    take_sample();
    assert(!mon.rate_valid);
    for (unsigned n = 0; n < 65; n++) {
        fixture.time_us += 1000000; fixture.busy_us += 250000;
        fixture.tasks[0].cpu_us += 50000; fixture.tasks[1].cpu_us += 200000;
        clock_ms += 1000;
        take_sample();
    }
    assert(mon.rate_valid && mon.cpu == 25 && mon.task_cpu[0] == 5 && mon.task_cpu[1] == 20);
    assert(mon.order[0] == 1 && mon.history_count == MON_HISTORY);
    assert(mon.mem_history[MON_HISTORY - 1] == 50);
    shot("performance");
    activate(0); assert(paused); shot("paused"); activate(0); assert(!paused);
    page = 1; shot("tasks");
    selected_pid = 65; page = 2; shot("detail");
    assert(gfx_rect_contains(end_rect, 120, 280));
    activate(5); assert(alert == 1 && stops == 0); shot("confirm");
    activate(6); assert(alert == 0 && stops == 0);
    activate(5); activate(7); assert(stops == 1 && stopped_pid == 65);
    activate(6);
    selected_pid = 33; shot("protected"); assert(gfx_rect_empty(end_rect));
    activate(5); assert(!alert && stops == 1);
    selected_pid = 97; shot("self"); assert(gfx_rect_empty(end_rect));
    /* Table slot reuse must not target the replacement or inherit CPU. */
    selected_pid = 65;
    fixture.tasks[1].pid = 129; fixture.tasks[1].cpu_us = 100;
    fixture.time_us += 1000000; fixture.busy_us += 1000; take_sample();
    assert(monitor_find(&mon, 65) == -1 && mon.task_cpu[1] == 0);
    shot("ended"); activate(5); assert(stops == 1);
    /* Moving a PID between slots retains that task's own CPU baseline. */
    struct gz_task_info tmp = fixture.tasks[0]; fixture.tasks[0] = fixture.tasks[2]; fixture.tasks[2] = tmp;
    fixture.time_us += 1000000; fixture.busy_us += 900000;
    fixture.tasks[0].cpu_us += 700000; take_sample();
    assert(mon.task_cpu[0] == 70 && mon.task_cpu[2] == 0);
    /* Duplicate/backwards timestamps cannot produce giant percentages. */
    take_sample(); assert(!mon.rate_valid);
    fixture.time_us--; take_sample(); assert(!mon.rate_valid);
    old_kernel = true; take_sample(); assert(!mon.available && !mon.now.count && !mon.rate_valid);
    page = 0; shot("legacy-performance"); page = 1; shot("legacy-tasks");
    assert(mon.now.total_pages == 4096 && mon.now.free_pages == 2048);
    puts("Task Manager sampling, PID identity, confirmation, protection and rendering pass");
    return 0;
}
