#ifndef WINCE_RUNTIME_H
#define WINCE_RUNTIME_H

#include "pe.h"
#include <glaze/fs.h>
#include <glaze/sys.h>
#include <glaze/ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef uint32_t CW;
typedef uint16_t WCHAR;
#define CE_WINDOWS 192
#define PTR(type, word) ((type *)(uintptr_t)(word))
#define WORD(ptr) ((CW)(uintptr_t)(ptr))
#define CE_API(name)                                                                                         \
    CW ce_##name(CW a0, CW a1, CW a2, CW a3, CW a4, CW a5, CW a6, CW a7, CW a8, CW a9, CW a10, CW a11)
/* clang-format off */
#define CE_EXPORT(name) { #name, (void *)ce_##name }
/* clang-format on */
struct ce_export {
    const char *name;
    void *address;
};

struct ce_module {
    struct ce_pe pe;
    void *image;
    CW base;
};
extern struct ce_module ce_main;
extern CW ce_error;
extern struct ui_keyboard ce_keyboard;
extern bool ce_keyboard_open;

struct ce_rect {
    int32_t left, top, right, bottom;
};
struct ce_message {
    CW window, message, wparam, lparam, time;
    int32_t x, y;
};
struct ce_window {
    CW handle, parent, id, proc, style, user;
    struct ce_rect rect;
    WCHAR *text;
    uint32_t length, capacity, cursor, sel_start, sel_end;
    uint32_t text_limit, checked;
    char klass[64];
    bool visible, dirty, modified, enabled;
    struct ce_tabs *tabs;
    struct ce_list *list;
    CW dialog_proc, dialog_result;
    CW dialog_message_result;
    bool property_page;
    CW spin_buddy;
    int32_t spin_low, spin_high;
    bool dialog_done;
    /* GWL_USERDATA is `user`; DWL_USER and the cbWndExtra longs are separate. */
    CW exstyle, dialog_user, extra[16];
};
struct ce_tabs {
    unsigned count;
    int selected;
    WCHAR text[16][64];
};
enum { CE_OBJ_BRUSH = 1, CE_OBJ_FONT, CE_OBJ_BITMAP, CE_OBJ_MENU, CE_OBJ_IMAGE_LIST, CE_OBJ_PEN };
struct ce_object {
    int kind;
    CW value;
    void *data;
    uint32_t bytes;
    int width, height;
};

void ce_fatal(const char *reason) __attribute__((noreturn));
void ce_missing(const char *symbol) __attribute__((noreturn));
void *ce_resolve_core(const char *);
void *ce_resolve_time(const char *);
void *ce_resolve_window(const char *);
void *ce_resolve_gdi(const char *);
void *ce_resolve_shell(const char *, const char *);
void ce_shell_draw(void);
bool ce_shell_input(const struct gz_input_event *);
CW ce_load_library(const char *);
CW ce_module_handle(const char *);
CW ce_find_symbol(CW, const char *);
CW ce_find_ordinal(CW, uint16_t);
CW ce_core_proc(const char *, uint16_t);
uint32_t ce_module_extent(CW);
bool ce_bundled_res_name(const char *leaf, char out[48]);
void ce_shutdown(void);
const void *ce_resource(CW module, uint16_t type, uint16_t id, uint32_t *size);
CW ce_call(CW proc, CW a0, CW a1, CW a2, CW a3);

unsigned ce_wlen(const WCHAR *);
void ce_narrow(char *, uint32_t, const WCHAR *);
void ce_widen(WCHAR *, uint32_t, const char *);
int ce_wcmp(const WCHAR *, const WCHAR *);
bool ce_path(char path[FS_PATH_MAX], const WCHAR *);
void ce_render(void);
void ce_redraw_all(void);
struct ce_dialog_template;
CW ce_dialog_controls(CW, const struct ce_dialog_template *, int);
CW ce_modal_run(CW);
void ce_destroy_window(CW);
void *ce_resolve_property(const char *);
uint32_t ce_registry_dword(const char *, const char *, uint32_t);
bool ce_registry_set_dword(const char *, const char *, uint32_t);
void ce_post(CW window, CW message, CW wparam, CW lparam);
CW ce_send(CW window, CW message, CW wparam, CW lparam);
struct ce_window *ce_window(CW);
bool ce_window_visible(CW);
struct ce_window *ce_new_window(const WCHAR *klass, const WCHAR *text, CW style, int x, int y, int w, int h,
                                CW parent, CW id, CW proc, CW arg);
struct ce_object *ce_object(CW, int);
CW ce_new_object(int, CW, void *, uint32_t);
gfx_rect ce_window_rect(CW);
uint32_t ce_color(CW colorref);
CW ce_draw_text(CW dc, CW text, CW count, CW rect, CW format);
void ce_draw_bitmap(const struct ce_object *, int x, int y, int source_x, int width, CW mask);
bool ce_list_init(struct ce_window *);
void ce_list_free(struct ce_window *);
bool ce_list_message(struct ce_window *, CW, CW, CW, CW *);
void ce_list_draw(struct ce_window *);
void ce_list_popup_draw(void);
bool ce_list_popup_input(const struct gz_input_event *);

#endif
