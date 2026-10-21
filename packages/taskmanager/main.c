/* Native task/performance monitor; one sample per second, no polling thread. */
#include "monitor.h"
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <string.h>

static struct monitor mon;
static struct gz_task_snapshot sample;
static struct ui_list tasks;
static uint32_t last_sample, selected_pid;
static bool paused, touching;
static int page, alert, pressed = -1; /* pages: performance, tasks, detail */
static gfx_rect pause_rect, back_rect, end_rect, alert_no, alert_yes;
static char alert_msg[96];

static void take_sample(void)
{
    bool available = gz_task_snapshot(&sample) == 0;
    if (!available) {
        struct gz_sysinfo si = {0};
        gz_sysinfo(&si);
        memset(&sample, 0, sizeof(sample));
        sample.time_us = (uint64_t)si.uptime_ms * 1000u;
        sample.total_pages = si.total_pages;
        sample.free_pages = si.free_pages;
        sample.page_size = 4096;
    }
    monitor_update(&mon, &sample, available);
    ui_list_set_count(&tasks, mon.now.count);
    last_sample = gz_time_ms();
}

static void text(gfx_surface *s, int x, int y, const char *str, uint32_t color)
{
    gfx_text(s, x, y, ui_font(FONT_R12), str, color, 255);
}

static void text_right(gfx_surface *s, int right, int y, const gfx_font *font,
                       const char *str, uint32_t color, int alpha)
{
    gfx_text(s, right - gfx_text_width(font, str), y, font, str, color, alpha);
}

static void memory(char *out, size_t cap, uint32_t pages)
{
    uint32_t kib = pages * (mon.now.page_size / 1024u);
    snprintf(out, cap, "%u.%u MB", kib / 1024u, (kib % 1024u) * 10u / 1024u);
}

static void graph(gfx_surface *s, int y, const uint8_t *history, uint32_t color)
{
    gfx_rect r = gfx_r(17, y, 206, 37);
    gfx_rect old = s->clip, clip;
    if (!gfx_rect_intersect(old, r, &clip)) return;
    s->clip = clip;
    gfx_fill(s, r, 0x132329);
    for (int i = 0; i <= 4; i++)
        gfx_fill(s, gfx_r(r.x, r.y + i * 9, r.w, 1), 0x2D4045);
    for (int i = 0; i < 6; i++)
        gfx_fill(s, gfx_r(r.x + i * 41, r.y, 1, r.h), 0x2D4045);
    int first = MON_HISTORY - (int)mon.history_count;
    for (int i = first; i < MON_HISTORY; i++) {
        if (history[i] > 100) continue;
        int x = r.x + i * (r.w - 1) / (MON_HISTORY - 1);
        int h = history[i] * (r.h - 2) / 100;
        gfx_fill(s, gfx_r(x, r.y + r.h - 1 - h, 3, h), color == 0x71E89A ? 0x27583A : 0x24566E);
        if (i > first && history[i - 1] <= 100)
            gfx_line(s, r.x + (i - 1) * (r.w - 1) / (MON_HISTORY - 1),
                     r.y + r.h - 1 - history[i - 1] * (r.h - 2) / 100,
                     x, r.y + r.h - 1 - h, color, 255);
    }
    s->clip = old;
}

static void draw_performance(gfx_surface *s)
{
    char buf[80], used[24], free_mem[24], total[24];
    uint32_t used_pages = mon.now.total_pages - MIN(mon.now.free_pages, mon.now.total_pages);
    ui_draw_group_cell(s, gfx_r(8, 76, 224, 77), 0, false);
    text(s, 17, 83, "CPU", UI_TEXT);
    if (!mon.available) strcpy(buf, "Unavailable");
    else if (!mon.rate_valid) strcpy(buf, "Sampling...");
    else snprintf(buf, sizeof(buf), "%u%% busy", mon.cpu);
    text_right(s, 223, 83, ui_font(FONT_B12), buf, UI_TEXT_DETAIL, 255);
    graph(s, 106, mon.cpu_history, 0x71E89A);
    ui_draw_group_cell(s, gfx_r(8, 161, 224, 77), 0, false);
    memory(used, sizeof(used), used_pages);
    memory(total, sizeof(total), mon.now.total_pages);
    memory(free_mem, sizeof(free_mem), mon.now.free_pages);
    text(s, 17, 168, "Memory", UI_TEXT);
    snprintf(buf, sizeof(buf), "%s used", used);
    text_right(s, 223, 168, ui_font(FONT_B12), buf, UI_TEXT_DETAIL, 255);
    graph(s, 191, mon.mem_history, 0x71CEF2);
    snprintf(buf, sizeof(buf), "%s free / %s total", free_mem, total);
    gfx_text_center(s, 0, 245, UI_W, ui_font(FONT_R10), buf, 0x4C566C, 255);
    uint32_t sec = (uint32_t)(mon.now.time_us / 1000000u);
    snprintf(buf, sizeof(buf), "Up %ud %02u:%02u:%02u", sec / 86400u, sec / 3600u % 24u, sec / 60u % 60u, sec % 60u);
    gfx_text_center(s, 0, 263, UI_W, ui_font(FONT_R12), buf, UI_TEXT_DETAIL, 255);
    snprintf(buf, sizeof(buf), paused ? "Paused" : "1 second samples  /  60 sample history");
    gfx_text_center(s, 0, 285, UI_W, ui_font(FONT_R10), buf, 0x4C566C, 255);
}

static void task_row(gfx_surface *s, int row, gfx_rect r, bool selected, void *ctx)
{
    unsigned i = mon.order[row];
    const struct gz_task_info *t = &mon.now.tasks[i];
    char buf[64];
    uint32_t color = selected ? 0xFFFFFF : UI_TEXT_DETAIL;
    ui_draw_list_row(s, r, selected);
    gfx_text_fit(s, 10, r.y + 4, 156, ui_font(FONT_B14), t->name, selected ? 0xFFFFFF : UI_TEXT, 255);
    if (mon.rate_valid) snprintf(buf, sizeof(buf), "%u%%", mon.task_cpu[i]);
    else strcpy(buf, "--");
    text_right(s, 222, r.y + 4, ui_font(FONT_B12), buf, color, 255);
    snprintf(buf, sizeof(buf), "PID %u  %s", t->pid, (t->flags & GZ_TASK_SELF) ? "This App" :
             (t->flags & GZ_TASK_BG) ? "Background App" : (t->flags & GZ_TASK_APP) ? "App" : "System");
    gfx_text(s, 10, r.y + 24, ui_font(FONT_R10), buf, color, 255);
    snprintf(buf, sizeof(buf), "%u KB", t->mapped_pages * (mon.now.page_size / 1024u));
    text_right(s, 222, r.y + 24, ui_font(FONT_R10), buf, color, 255);
}

static void draw_tasks(gfx_surface *s)
{
    char buf[64];
    if (!mon.available) {
        gfx_text_center(s, 0, 112, UI_W, ui_font(FONT_B14), "OS Update Required", UI_TEXT, 255);
        gfx_text_center(s, 0, 143, UI_W, ui_font(FONT_R12), "Update Glaze for CPU monitoring", UI_TEXT_DETAIL, 255);
        gfx_text_center(s, 0, 161, UI_W, ui_font(FONT_R12), "and task management.", UI_TEXT_DETAIL, 255);
        return;
    }
    gfx_fill(s, gfx_r(0, 72, UI_W, UI_H - 72), 0xFFFFFF);
    gfx_vgradient(s, gfx_r(0, 72, UI_W, 24), 0xE7ECF2, 0xCCD4DF, 255);
    snprintf(buf, sizeof(buf), "%u tasks", mon.now.count);
    text(s, 10, 77, buf, UI_TEXT_DETAIL);
    text_right(s, 228, 77, ui_font(FONT_R10), "CPU / Mapped RAM", UI_TEXT_DETAIL, 255);
    ui_list_draw(s, &tasks, task_row, NULL);
}

static void detail_row(gfx_surface *s, int y, const char *label, const char *value, int pos)
{
    ui_draw_group_cell(s, gfx_r(8, y, 224, 27), pos, false);
    text(s, 17, y + 6, label, UI_TEXT);
    text_right(s, 222, y + 6, ui_font(FONT_R12), value, UI_TEXT_DETAIL, 255);
}

static void draw_detail(gfx_surface *s)
{
    char buf[64];
    int i = monitor_find(&mon, selected_pid);
    end_rect = gfx_r(0, 0, 0, 0);
    if (i < 0) {
        gfx_text_center(s, 0, 125, UI_W, ui_font(FONT_B14), "Task has ended", UI_TEXT_DETAIL, 255);
        return;
    }
    const struct gz_task_info *t = &mon.now.tasks[i];
    gfx_text_fit(s, 12, 45, 216, ui_font(FONT_B20), t->name, UI_TEXT, 255);
    snprintf(buf, sizeof(buf), "%u", t->pid);
    detail_row(s, 78, "Process ID", buf, 1);
    snprintf(buf, sizeof(buf), "%u", t->parent);
    detail_row(s, 105, "Parent ID", buf, 2);
    snprintf(buf, sizeof(buf), "%u (%u ready)", t->threads, t->runnable);
    detail_row(s, 132, "Threads", buf, 2);
    snprintf(buf, sizeof(buf), "%u KB", t->mapped_pages * (mon.now.page_size / 1024u));
    detail_row(s, 159, "Mapped RAM", buf, 2);
    if (mon.rate_valid) snprintf(buf, sizeof(buf), "%u%%", mon.task_cpu[i]);
    else strcpy(buf, "Sampling...");
    detail_row(s, 186, "CPU", buf, 2);
    snprintf(buf, sizeof(buf), "%u.%03u s", (uint32_t)(t->cpu_us / 1000000u), (uint32_t)(t->cpu_us / 1000u % 1000u));
    detail_row(s, 213, "CPU Time", buf, 3);
    gfx_text_center(s, 0, 246, UI_W, ui_font(FONT_R10), "Mapped RAM includes shared pages.", 0x4C566C, 255);
    if (monitor_can_stop(t)) {
        end_rect = gfx_r(40, 267, 160, 29);
        gfx_round_rect_vgradient(s, end_rect, 6, pressed == 5 ? 0x9A302A : 0xEF6D65, 0xA82620, 255);
        gfx_text_center(s, end_rect.x, end_rect.y + 7, end_rect.w, ui_font(FONT_B14), "End Task", 0xFFFFFF, 255);
    } else {
        gfx_text_center(s, 0, 276, UI_W, ui_font(FONT_B12),
                        t->flags & GZ_TASK_SELF ? "This App" : "Protected System Task", UI_TEXT_DETAIL, 255);
    }
}

static void draw(void)
{
    gfx_surface *s = &ui.surf;
    ui_draw_pinstripes(s, gfx_r(0, 0, UI_W, UI_H));
    ui_draw_navbar(s, page == 2 ? "Task Details" : "Task Manager", page == 2 ? "Tasks" : NULL, &back_rect, 0);
    pause_rect = ui_draw_bar_button(s, paused ? "Live" : "Pause", true, pressed == 0, false, 0);
    if (page == 2) draw_detail(s);
    else {
        for (int i = 0; i < 2; i++) {
            gfx_rect r = gfx_r(8 + 112 * i, 39, 112, 27);
            gfx_round_rect(s, r, 5, 0x71849A, 255);
            gfx_round_rect_vgradient(s, gfx_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 4,
                                    page == i ? 0x5F86B8 : 0xFFFFFF, page == i ? 0x315C90 : 0xD4DFEA, 255);
            gfx_text_center(s, r.x, r.y + 6, r.w, ui_font(FONT_B12), i ? "Tasks" : "Performance",
                            page == i ? 0xFFFFFF : UI_TEXT_DETAIL, 255);
        }
        if (page == 0) draw_performance(s);
        else draw_tasks(s);
    }
    if (alert)
        ui_draw_alert(s, alert == 1 ? "End Task?" : "Task Manager", alert_msg,
                      alert == 1 ? "Cancel" : "OK", alert == 1 ? "End Task" : NULL,
                      &alert_no, &alert_yes, pressed == 6 ? 0 : pressed == 7 ? 1 : -1);
    ui_invalidate_all();
    ui_present();
}

static int hit(int x, int y)
{
    if (alert) {
        if (gfx_rect_contains(alert_no, x, y)) return 6;
        if (alert == 1 && gfx_rect_contains(alert_yes, x, y)) return 7;
        return -1;
    }
    if (gfx_rect_contains(pause_rect, x, y)) return 0;
    if (page == 2) {
        if (gfx_rect_contains(back_rect, x, y)) return 1;
        if (gfx_rect_contains(end_rect, x, y)) return 5;
    } else if (y >= 39 && y < 66 && x >= 8 && x < 232) return x < 120 ? 2 : 3;
    return -1;
}

static void activate(int control)
{
    if (control == 0) { paused = !paused; if (!paused) take_sample(); }
    else if (control == 1) page = 1;
    else if (control == 2 || control == 3) page = control - 2;
    else if (control == 5) {
        int i = monitor_find(&mon, selected_pid);
        if (i >= 0 && monitor_can_stop(&mon.now.tasks[i])) {
            alert = 1;
            strcpy(alert_msg, "Unsaved work may be lost.\nEnd this app now?");
        }
    } else if (control == 6) alert = 0;
    else if (control == 7) {
        int result = gz_task_stop((int)selected_pid);
        alert = 2;
        strcpy(alert_msg, result == 0 ? "Task ended." : result == GZ_ENOENT ? "Task already ended." : "This task cannot be ended.");
        take_sample();
    }
}

int main(void)
{
    ui_init(SB_STATUS_GRAY);
    ui_list_init(&tasks, gfx_r(0, 96, UI_W, UI_H - 96), 44);
    take_sample();
    draw();
    for (;;) {
        struct gz_input_event ev;
        bool dirty = false;
        if (ui_next_event(&ev, 50)) {
            if (!alert && page == 1 && mon.available) {
                int row = ui_list_event(&tasks, &ev, &dirty);
                if (row >= 0) { selected_pid = mon.now.tasks[mon.order[row]].pid; page = 2; dirty = true; }
            }
            if (ev.type == GZ_EV_TOUCH_DOWN) { touching = true; pressed = hit(ev.x, ev.y); dirty = true; }
            else if (ev.type == GZ_EV_TOUCH_MOVE) {
                if (pressed >= 0 && hit(ev.x, ev.y) != pressed) { pressed = -1; dirty = true; }
            } else if (ev.type == GZ_EV_TOUCH_UP) {
                touching = false;
                if (pressed >= 0 && hit(ev.x, ev.y) == pressed) activate(pressed);
                pressed = -1;
                dirty = true;
            } else if (ev.type == GZ_EV_KEY_DOWN && ev.code == GZ_KEY_ESC) {
                if (alert) alert = 0;
                else if (page == 2) page = 1;
                else page = 0;
                dirty = true;
            }
        }
        if (page == 1 && ui_scroll_step(&tasks.sc)) dirty = true;
        if (!paused && !touching && !alert && (uint32_t)(gz_time_ms() - last_sample) >= 1000) {
            take_sample();
            dirty = true;
        }
        if (dirty) draw();
    }
}
