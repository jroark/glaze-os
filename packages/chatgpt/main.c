/* Direct Codex chat. Account authorization and HTTP belong to the worker. */
#include "codex.h"
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct chat_history history = {.version = 1};
static struct chat_config config = {.version = 2, .model = "gpt-6-luna"};
static char draft[CHAT_DRAFT], notice[CHAT_TEXT], live[CHAT_TEXT], device_code[40], plan[40];
static bool account_view, editing, busy, signing_in, signed_in, worker_ok, failed, confirm_clear, draft_dirty;
static int focus, cursor[2], pressed;
static uint32_t last_edit;
static struct ui_keyboard keyboard;
static struct ui_scroll scroll;
static int lines[CHAT_COUNT][512], nlines[CHAT_COUNT], offsets[CHAT_COUNT];
static gfx_rect left_button, right_button, send_button, compose, model_field, keep, login_button, import_button, alert[2];
static void path(char *out, const char *name) { snprintf(out, FS_PATH_MAX, "%s/data/%s", res_bundle(), name); }
static bool write_file(const char *name, const void *data, uint32_t len)
{
    char dest[FS_PATH_MAX], temp[FS_PATH_MAX]; path(dest, name); snprintf(temp, sizeof(temp), "%s.tmp", dest);
    if (fs_write_file(temp, data, len) != (int)len || fs_rename(temp, dest) < 0) {
        snprintf(notice, sizeof(notice), "Could not save. Check free storage."); return false;
    }
    return true;
}
static void save_chat(void) { write_file("history", &history, 8 + history.count * sizeof(history.messages[0])); }
static void save_draft(void) { if (draft_dirty && write_file("draft", draft, (uint32_t)strlen(draft) + 1)) draft_dirty = false; }
static void save_config(void) { write_file("config", &config, sizeof(config)); }
static void load(void)
{
    char p[FS_PATH_MAX]; snprintf(p, sizeof(p), "%s/data", res_bundle()); fs_mkdirs(p);
    struct chat_config saved;
    path(p, "config");
    if (fs_read_file(p, &saved, sizeof(saved)) == (int)sizeof(saved) && saved.version == 2 && saved.remember <= 1 && chat_model_ok(saved.model)) config = saved;
    path(p, "history"); int n = fs_read_file(p, &history, sizeof(history));
    if (n < 8 || !chat_history_valid(&history) || n != (int)(8 + history.count * sizeof(history.messages[0]))) {
        memset(&history, 0, sizeof(history)); history.version = 1;
    }
    path(p, "draft"); n = fs_read_file(p, draft, sizeof(draft));
    if (n < 1 || draft[n - 1] || !memchr(draft, 0, sizeof(draft))) draft[0] = 0;
    cursor[0] = (int)strlen(draft); cursor[1] = (int)strlen(config.model);
    account_view = true; failed = (history.count & 1) != 0;
}
static int bottom(void) { return UI_H - (editing ? ui_keyboard_height() : 0); }
static void layout(bool end)
{
    int height = 8;
    for (unsigned i = 0; i < history.count; i++) {
        nlines[i] = ui_wrap(ui_font(FONT_R13), history.messages[i].text, 200, lines[i], 512);
        offsets[i] = height; height += 25 + nlines[i] * ui_font(FONT_R13)->line_height + 10;
    }
    if (live[0]) { int starts[512]; height += 25 + ui_wrap(ui_font(FONT_R13), live, 200, starts, 512) * ui_font(FONT_R13)->line_height + 10; }
    scroll.content_h = height; scroll.view_h = MAX(1, bottom() - UI_NAV_H - 66 - (notice[0] && !busy ? 44 : 0));
    scroll.offset = end ? ui_scroll_max(&scroll) : MIN(scroll.offset, ui_scroll_max(&scroll));
}
static void text_field(gfx_rect r, const char *text, int index, const char *hint)
{
    gfx_surface *s = &ui.surf; gfx_round_rect(s, r, 5, 0xFFFFFF, 255);
    const gfx_font *f = ui_font(FONT_R13);
    int cur = MIN(cursor[index], (int)strlen(text)), x = gfx_text_width_n(f, text, cur);
    int shift = editing && focus == index ? MAX(0, x - r.w + 17) : 0;
    gfx_rect old = s->clip, clip;
    if (!gfx_rect_intersect(old, gfx_r(r.x + 5, r.y, r.w - 10, r.h), &clip)) clip = gfx_r(0, 0, 0, 0);
    gfx_set_clip(s, clip);
    if (editing && focus == index) ui_draw_edit_selection(s, f, text, cursor[index], 0, strlen(text), r.x + 6 - shift, r.y + 7, f->line_height);
    gfx_text(s, r.x + 6 - shift, r.y + 7, f, *text ? text : hint, *text ? UI_TEXT : 0x858B94, 255);
    if (editing && focus == index) gfx_fill(s, gfx_r(r.x + 6 + x - shift, r.y + 6, 1, 17), 0x2A6CD9);
    s->clip = old;
}
static void bubble(gfx_surface *s, int y, const char *text, const int *starts, int count, bool assistant)
{
    int h = 25 + count * ui_font(FONT_R13)->line_height;
    if (y + h < UI_NAV_H || y > UI_NAV_H + scroll.view_h) return;
    gfx_round_rect(s, gfx_r(8, y, 224, h), 6, assistant ? 0xFFFFFF : 0xD8EDE5, 255);
    gfx_text(s, 17, y + 5, ui_font(FONT_B11), assistant ? "Codex" : "You", 0x35675A, 255);
    for (int j = 0; j < count; j++)
        gfx_text_n(s, 17, y + 21 + j * ui_font(FONT_R13)->line_height, ui_font(FONT_R13), text + starts[j],
                   ui_line_len(text, starts, count, j), UI_TEXT, 255);
}
static void draw(void)
{
    gfx_surface *s = &ui.surf; gfx_fill(s, gfx_r(0, 0, UI_W, UI_H), 0xEEF2F4);
    ui_draw_navbar(s, account_view ? "ChatGPT login" : "Chat", NULL, NULL, 0);
    left_button = ui_draw_bar_button(s, account_view ? "Back" : "New", false, pressed == 1, false, 0);
    right_button = ui_draw_bar_button(s, account_view ? "Done" : "Account", true, pressed == 2, false, 0);
    if (account_view) {
        gfx_rect old = s->clip; gfx_set_clip(s, gfx_r(0, UI_NAV_H, UI_W, bottom() - UI_NAV_H - 24));
        if (signing_in) {
            gfx_text_center(s, 0, 44, UI_W, ui_font(FONT_B14), device_code[0] ? "Authorize on your phone or PC" : "Contacting OpenAI...", UI_TEXT, 255);
            gfx_text_center(s, 2, 73, 236, ui_font(FONT_R10), CX_VERIFY, 0x385487, 255);
            gfx_text_center(s, 0, 102, UI_W, ui_font(FONT_B20), device_code, 0x256552, 255);
            gfx_text_center(s, 0, 138, UI_W, ui_font(FONT_R11), "Enter this code after signing in.", 0x536776, 255);
            gfx_text_center(s, 0, 158, UI_W, ui_font(FONT_R10), "The code expires in 15 minutes.", 0x536776, 255);
            login_button = gfx_r(30, 198, 180, 32); ui_draw_button(s, login_button, "Cancel sign-in", pressed == 4);
        } else {
            char label[96]; snprintf(label, sizeof(label), signed_in ? "Signed in to Codex%s%s" : "Sign in with your ChatGPT account", plan[0] ? " - " : "", plan);
            gfx_text_center(s, 4, 41, 232, ui_font(FONT_B12), label, 0x35675A, 255);
            gfx_text(s, 10, 67, ui_font(FONT_B12), "Model", 0x385487, 255);
            model_field = gfx_r(8, 85, 224, 30); text_field(model_field, config.model, 1, "Codex model name");
            if (!editing) {
                login_button = gfx_r(8, 127, 224, 32);
                import_button = gfx_r(8, 168, 224, 32);
                ui_draw_button(s, login_button, signed_in ? "Sign out" : "Sign in with ChatGPT", pressed == 4);
                if (!signed_in) ui_draw_button(s, import_button, "Import Codex login", pressed == 5);
                keep = gfx_r(8, 211, 224, 32);
                gfx_text(s, 12, 220, ui_font(FONT_R12), "Keep signed in", UI_TEXT, 255);
                ui_draw_switch(s, 160, 216, config.remember);
                gfx_text_center(s, 4, 252, 232, ui_font(FONT_R10), signed_in ? "Sign out to change saved-login settings." : "Saved sign-in uses this device's storage.", 0x687482, 255);
            } else keep = login_button = import_button = gfx_r(0, 0, 0, 0);
        }
        s->clip = old;
        if (notice[0] && !editing && !signing_in) {
            int starts[4], n = ui_wrap(ui_font(FONT_R10), notice, 220, starts, 4);
            gfx_fill(s, gfx_r(4, 244, 232, 39), 0xEEF2F4);
            for (int i = 0; i < MIN(n, 3); i++) gfx_text_n(s, 8, 245 + 12 * i, ui_font(FONT_R10), notice + starts[i], ui_line_len(notice, starts, n, i), 0x8C3E34, 255);
        }
    } else {
        gfx_rect old = s->clip; gfx_set_clip(s, gfx_r(0, UI_NAV_H, UI_W, scroll.view_h));
        if (!history.count) {
            gfx_text_center(s, 0, 66, UI_W, ui_font(FONT_B17), "What can I help with?", 0x304C49, 255);
            gfx_text_center(s, 0, 93, UI_W, ui_font(FONT_R12), signed_in ? "Write a message below." : "Open Account to sign in.", 0x687482, 255);
        }
        for (unsigned i = 0; i < history.count; i++) bubble(s, UI_NAV_H + offsets[i] - scroll.offset, history.messages[i].text, lines[i], nlines[i], history.messages[i].assistant);
        if (live[0]) {
            int starts[512], n = ui_wrap(ui_font(FONT_R13), live, 200, starts, 512);
            int y = history.count ? offsets[history.count - 1] + 35 + nlines[history.count - 1] * ui_font(FONT_R13)->line_height : 8;
            bubble(s, UI_NAV_H + y - scroll.offset, live, starts, n, true);
        }
        s->clip = old;
        compose = gfx_r(6, bottom() - 58, 164, 31); text_field(compose, draft, 0, "Message...");
        send_button = gfx_r(175, bottom() - 58, 60, 31);
        ui_draw_button(s, send_button, busy ? "Stop" : failed ? "Retry" : "Send", pressed == 3);
        if (notice[0] && !busy) {
            int starts[4], n = ui_wrap(ui_font(FONT_R10), notice, 220, starts, 4);
            for (int i = 0; i < MIN(n, 3); i++) gfx_text_n(s, 10, bottom() - 103 + i * 12, ui_font(FONT_R10), notice + starts[i], ui_line_len(notice, starts, n, i), failed ? 0xA02E28 : 0x536776, 255);
        }
    }
    const char *status = busy ? signing_in ? "Waiting for authorization..." : "Thinking..." : !account_view && failed ? "Retry resends the unanswered message." : "";
    gfx_text_fit(s, 6, bottom() - 21, 228, ui_font(FONT_R10), status, 0x536776, 255);
    if (editing && ui_keyboard_height()) ui_draw_keyboard(s, &keyboard, UI_H - UI_KB_H);
    if (confirm_clear) ui_draw_alert(s, "New chat?", "Clear the saved conversation?\nYour sign-in will stay.", "Cancel", "Clear", &alert[0], &alert[1], -1);
    ui_invalidate_all(); ui_present();
}
static void focus_field(int index)
{
    editing = true; focus = index; ui_keyboard_edit_init(&keyboard, 0); keyboard.url = false;
    if (index == 1) cursor[1] = (int)strlen(config.model);
    layout(true);
}
static void cancel(void)
{
    chat_worker_cancel(); busy = signing_in = false; live[0] = device_code[0] = 0; failed = (history.count & 1) != 0;
    snprintf(notice, sizeof(notice), "Stopped. An interrupted token refresh may require signing in again."); layout(false);
}
static void command(int kind)
{
    if (!worker_ok || !chat_worker_command(kind, &config)) { snprintf(notice, sizeof(notice), "Cannot start sign-in. Close another app and reopen Chat."); return; }
    save_config(); busy = true; signing_in = kind == CHAT_LOGIN; device_code[0] = live[0] = notice[0] = 0; editing = false;
}
static void send_message(void)
{
    if (busy) { cancel(); return; }
    if (!signed_in) { account_view = true; snprintf(notice, sizeof(notice), "Sign in with ChatGPT, or import a Codex login."); return; }
    if (!worker_ok || !chat_model_ok(config.model)) { snprintf(notice, sizeof(notice), "Check the model name in Account."); return; }
    bool added = false;
    if (!(history.count & 1)) {
        bool nonspace = false; for (const char *p = draft; *p; p++) if (*p != ' ' && *p != '\n' && *p != '\t') nonspace = true;
        if (!nonspace || !chat_append(&history, false, draft)) return;
        added = true;
    }
    char *body = malloc(CHAT_WIRE); int len = body ? chat_codex_request(&history, config.model, body, CHAT_WIRE) : -1;
    if (len < 0 || !chat_worker_send(&config, body, len)) { free(body); failed = true; snprintf(notice, sizeof(notice), "Not enough memory. Close another app and Retry."); }
    else { if (added) { draft[0] = 0; cursor[0] = 0; draft_dirty = true; save_draft(); } busy = true; failed = false; notice[0] = live[0] = 0; }
    editing = false; layout(true); save_chat();
}
static void input(const struct gz_input_event *ev)
{
    if (ev->type == GZ_EV_APP_DEACTIVATE) { if (busy) cancel(); pressed = 0; keyboard.pressed = -1; scroll.dragging = false; scroll.velocity = 0; save_draft(); save_config(); save_chat(); return; }
    if (ev->type == GZ_EV_TOUCH_CANCEL) { pressed = 0; keyboard.pressed = -1; scroll.dragging = false; scroll.velocity = 0; return; }
    if (confirm_clear) {
        if (ev->type == GZ_EV_TOUCH_DOWN) pressed = gfx_rect_contains(alert[0], ev->x, ev->y) ? 6 : gfx_rect_contains(alert[1], ev->x, ev->y) ? 7 : 0;
        if (ev->type == GZ_EV_TOUCH_UP) {
            if (pressed == 7 && gfx_rect_contains(alert[1], ev->x, ev->y)) { if (busy) cancel(); memset(&history, 0, sizeof(history)); history.version = 1; draft[0] = 0; cursor[0] = 0; draft_dirty = true; failed = false; notice[0] = 0; save_chat(); save_draft(); layout(true); confirm_clear = false; }
            else if (pressed == 6 && gfx_rect_contains(alert[0], ev->x, ev->y)) confirm_clear = false;
            pressed = 0;
        }
        return;
    }
    if (ev->type == GZ_EV_KEY_DOWN && ev->code == GZ_KEY_ESC) { editing = false; layout(false); return; }
    if (editing && ui_keyboard_height() && ev->type <= GZ_EV_TOUCH_UP && ev->y >= UI_H - UI_KB_H) {
        bool changed = false; int ch = ui_keyboard_event(&keyboard, ev, UI_H - UI_KB_H, &changed);
        if (ch && ch != UI_KEY_DOTCOM) { struct gz_input_event text = {.type = ch == '\n' || ch == '\b' ? GZ_EV_KEY_DOWN : GZ_EV_TEXT, .code = ch == '\n' ? GZ_KEY_ENTER : ch == '\b' ? GZ_KEY_BACKSPACE : (uint16_t)ch}; input(&text); }
        return;
    }
    if (!account_view && ev->type == GZ_EV_KEY_DOWN && (ev->code == GZ_KEY_ENTER || ev->code == GZ_KEY_OK)) { send_message(); return; }
    if (!account_view && !editing && (ev->type == GZ_EV_TEXT || ui_input_char(ev))) focus_field(0);
    if (editing) {
        char *text = focus == 0 ? draft : config.model; int cap = focus == 0 ? CHAT_DRAFT : (int)sizeof(config.model);
        if (ui_edit_input(text, cap, &cursor[focus], ev, false)) { if (!focus) { draft_dirty = true; last_edit = gz_time_ms(); } return; }
        if (account_view && ev->type == GZ_EV_KEY_DOWN && ev->code == GZ_KEY_ENTER) { editing = false; return; }
    }
    if (!account_view && !editing && ev->type == GZ_EV_KEY_DOWN && (ev->code == GZ_KEY_UP || ev->code == GZ_KEY_DOWN)) { scroll.offset = MAX(0, MIN(ui_scroll_max(&scroll), scroll.offset + (ev->code == GZ_KEY_UP ? -60 : 60))); return; }
    if (ev->type == GZ_EV_TOUCH_DOWN) pressed = gfx_rect_contains(left_button, ev->x, ev->y) ? 1 : gfx_rect_contains(right_button, ev->x, ev->y) ? 2 :
        !account_view && gfx_rect_contains(send_button, ev->x, ev->y) ? 3 : account_view && gfx_rect_contains(login_button, ev->x, ev->y) ? 4 :
        account_view && !signed_in && !signing_in && gfx_rect_contains(import_button, ev->x, ev->y) ? 5 : 0;
    if (ev->type == GZ_EV_TOUCH_UP) {
        int hit = pressed; pressed = 0;
        if (hit == 1 && gfx_rect_contains(left_button, ev->x, ev->y)) { if (account_view) { if (signing_in) cancel(); account_view = editing = false; layout(true); } else { editing = false; confirm_clear = true; } return; }
        if (hit == 2 && gfx_rect_contains(right_button, ev->x, ev->y)) {
            if (account_view) { if (!chat_model_ok(config.model)) snprintf(notice, sizeof(notice), "Enter a valid Codex model name."); else { if (signing_in) cancel(); save_config(); account_view = editing = false; notice[0] = 0; layout(true); } }
            else { if (busy) cancel(); account_view = true; editing = false; } return;
        }
        if (hit == 3 && gfx_rect_contains(send_button, ev->x, ev->y)) { send_message(); return; }
        if (hit == 4 && gfx_rect_contains(login_button, ev->x, ev->y)) { if (signing_in) cancel(); else if (!busy) command(signed_in ? CHAT_LOGOUT : CHAT_LOGIN); return; }
        if (hit == 5 && gfx_rect_contains(import_button, ev->x, ev->y) && !busy) { command(CHAT_IMPORT); return; }
        if (account_view && !signing_in) {
            if (gfx_rect_contains(model_field, ev->x, ev->y) && !busy) focus_field(1);
            else if (gfx_rect_contains(keep, ev->x, ev->y) && !signed_in && !busy) { config.remember ^= 1; save_config(); }
        } else if (!account_view && gfx_rect_contains(compose, ev->x, ev->y)) focus_field(0);
    }
    if (!account_view && ev->y >= UI_NAV_H && ev->y < UI_NAV_H + scroll.view_h) ui_scroll_event(&scroll, ev, ev->y - UI_NAV_H);
}
int main(void)
{
    ui_init(SB_STATUS_GRAY); ui_keyboard_init(&keyboard); load(); worker_ok = chat_worker_init(&config); layout(true); puts("chatgpt: ready"); draw();
    static struct chat_result result;
    for (;;) {
        struct gz_input_event ev; bool dirty = ui_next_event(&ev, 40) != 0; if (dirty) input(&ev);
        if (chat_worker_poll(&result)) {
            if (result.kind == CHAT_DEVICE) { snprintf(device_code, sizeof(device_code), "%.39s", result.text); signing_in = true; }
            else if (result.kind == CHAT_PARTIAL) snprintf(live, sizeof(live), "%s", result.text);
            else {
                busy = signing_in = false; device_code[0] = live[0] = 0; signed_in = result.signed_in;
                snprintf(plan, sizeof(plan), "%s", result.plan);
                if (result.kind == CHAT_SEND) {
                    failed = !result.success;
                    if (result.success && chat_append(&history, true, result.text)) { notice[0] = 0; save_chat(); puts("chatgpt: reply received"); }
                    else { failed = true; snprintf(notice, sizeof(notice), "%s", result.text); puts("chatgpt: request failed"); }
                } else { snprintf(notice, sizeof(notice), "%s", result.kind == CHAT_BOOT ? "" : result.text); puts(signed_in ? "chatgpt: signed in" : "chatgpt: signed out"); }
            }
            layout(true); dirty = true;
        }
        if (!account_view && ui_scroll_step(&scroll)) dirty = true;
        if (draft_dirty && gz_time_ms() - last_edit >= 1500) save_draft();
        if (dirty && !ui_in_background()) draw();
    }
}
