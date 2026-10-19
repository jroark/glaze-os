/* Identical UI workload for static/dynamic RAM comparisons (MEM_TESTS=1). */
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
static uint16_t pixels[UI_W * UI_H];
static void draw(void)
{
    gfx_surface s; struct ui_keyboard keyboard;
    gfx_surface_init(&s, pixels, UI_W, UI_H, UI_W);
    ui_draw_pinstripes(&s, gfx_r(0,0,UI_W,UI_H));
    ui_draw_navbar(&s, "Shared library test", NULL, NULL, 0);
    ui_keyboard_init(&keyboard); ui_draw_keyboard(&s, &keyboard, UI_H - UI_KB_H);
    gfx_text(&s, 8, 50, ui_font(FONT_R14), "Same UI in three processes", 0, 255);
}
int main(void)
{
    draw();
    struct gz_msg m = {{0}}, r;
    int port = gz_port_lookup("ldmem");
    if (port > 0) { gz_send(port, &m, &r); return 0; }
    port = gz_port_create(); gz_port_register(port, "ldmem");
    int pids[2], tokens[2];
    for (int i = 0; i < 2; i++) pids[i] = gz_spawn("bin/ldmem", 0);
    for (int i = 0; i < 2; i++) tokens[i] = gz_recv(port, &m, 5000);
    struct gz_sysinfo si; struct gz_image_stats im;
    gz_sysinfo(&si); gz_image_stats(&im);
    printf("ldmem: free_pages=%u processes=%u mappings=%u shared_pages=%u private_pages=%u\n",
           si.free_pages, si.nprocs, im.mappings, im.shared_pages, im.private_pages);
    for (int i = 0; i < 2; i++) if (tokens[i] > 0) gz_reply(tokens[i], &m);
    if (pids[0] <= 0 || pids[1] <= 0 || tokens[0] <= 0 || tokens[1] <= 0) { puts("ldmem: FAILED"); return 1; }
    puts("ldmem: ALL PASS"); return 0;
}
