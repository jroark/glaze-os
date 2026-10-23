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
CW *ce_error_slot(void);
#define ce_error (*ce_error_slot())
void *ce_resolve_thread(const char *);
void *ce_resolve_ole(const char *, const char *);
void *ce_resolve_winsock(const char *, const char *);
bool ce_sync_close(CW, bool *);
bool ce_mapping_close(CW, bool *);
bool ce_database_close(CW, bool *);
void *ce_resolve_database(const char *);
void *ce_resolve_mapping(const char *);
void *ce_resolve_string(const char *);
void *ce_resolve_scan(const char *);
void **ce_token_context(unsigned);
bool ce_file_times(const char *, uint8_t [24], bool);
bool ce_file_set_times(const char *, const uint8_t [24]);
bool ce_file_touch(const char *, bool);
bool ce_file_flush_times(void);
bool ce_file_move_times(const char *, const char *);
void ce_thread_exit(CW) __attribute__((noreturn));
extern struct ui_keyboard ce_keyboard;
extern bool ce_keyboard_open;
extern bool ce_screen_dirty; /* drawn outside WM_PAINT; present on idle */

struct ce_rect {
    int32_t left, top, right, bottom;
};
struct ce_message {
    CW window, message, wparam, lparam, time;
    int32_t x, y;
};
struct ce_window {
    CW handle, parent, id, proc, style, user, thread, serial;
    struct ce_rect rect;
    WCHAR *text;
    uint32_t length, capacity, cursor, sel_start, sel_end;
    uint32_t text_limit, checked;
    char klass[64];
    bool visible, dirty, modified, enabled;
    struct ce_tabs *tabs;
    struct ce_list *list;
    struct ce_view *view;
    CW dialog_proc, dialog_result;
    CW dialog_message_result;
    CW dialog_user, extra[16];
    CW exstyle;
    bool property_page;
    CW spin_buddy;
    CW combo_edit;
    unsigned bar_images;
    int32_t spin_low, spin_high;
    bool dialog_done;
    CW menu;
    unsigned client_top;
    bool nonclient;
    struct {
        uint16_t value[8], minimum[8], maximum[8], format[64];
        unsigned range;
        bool initialized, none;
    } date;
    struct {
        int32_t minimum, maximum, position, track;
        uint32_t page;
    } scroll[2];
};
struct ce_tabs {
    unsigned count;
    int selected;
    WCHAR text[16][64];
    CW param[16], image[16], state[16];
};
enum { CE_OBJ_BRUSH = 1, CE_OBJ_FONT, CE_OBJ_BITMAP, CE_OBJ_MENU, CE_OBJ_IMAGE_LIST, CE_OBJ_PEN, CE_OBJ_ICON, CE_OBJ_DC, CE_OBJ_REGION, CE_OBJ_PALETTE };
struct ce_object {
    int kind;
    CW value;
    void *data;
    uint32_t bytes;
    int width, height;
    bool stock; /* DeleteObject leaves stock objects alone */
};

void ce_fatal(const char *reason) __attribute__((noreturn));
void ce_missing(const char *symbol) __attribute__((noreturn));
void *ce_resolve_core(const char *);
void *ce_resolve_crt(const char *);
void *ce_resolve_time(const char *);
void *ce_resolve_timezone(const char *);
void *ce_resolve_window(const char *);
void *ce_resolve_gdi(const char *);
void *ce_resolve_shell(const char *, const char *);
void ce_shell_draw(void);
bool ce_shell_input(const struct gz_input_event *);
CW ce_load_library(const char *);
CW ce_module_handle(const char *);
CW ce_find_symbol(CW, const char *);
CW ce_find_ordinal(CW, uint16_t);
CW ce_import_address(const char *, const char *);
bool ce_bundled_res_name(const char *leaf, char out[48]);
void ce_shutdown(void);
const void *ce_resource(CW module, uint16_t type, uint16_t id, uint32_t *size);
uint32_t ce_image_remaining(CW address);
const char *ce_module_name(CW handle);
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
CW ce_dialog_proc(CW, CW, CW, CW);
bool ce_modal_allows(CW);
bool ce_date_message(struct ce_window *, CW, CW, CW, CW *);
void ce_date_text(struct ce_window *, WCHAR *, unsigned);
CW ce_tabs_message(struct ce_window *, CW, CW, CW, bool *);
bool ce_shell_attach_menu(CW, CW, CW);
gfx_rect ce_client_rect(CW);
struct ce_window *ce_window(CW);
bool ce_window_visible(CW);
struct ce_window *ce_new_window(const WCHAR *klass, const WCHAR *text, CW style, int x, int y, int w, int h,
                                CW parent, CW id, CW proc, CW arg);
struct ce_object *ce_object(CW, int);
CW ce_new_object(int, CW, void *, uint32_t);
gfx_rect ce_window_rect(CW);
uint32_t ce_color(CW colorref);
CW ce_draw_text(CW dc, CW text, CW count, CW rect, CW format);
bool ce_list_init(struct ce_window *);
void ce_list_free(struct ce_window *);
bool ce_list_message(struct ce_window *, CW, CW, CW, CW *);
void ce_list_draw(struct ce_window *);
void ce_list_popup_draw(void);
bool ce_list_popup_input(const struct gz_input_event *);
bool ce_view_init(struct ce_window *);
void ce_view_free(struct ce_window *);
bool ce_view_message(struct ce_window *, CW, CW, CW, CW *);
void ce_view_draw(struct ce_window *);

#endif
