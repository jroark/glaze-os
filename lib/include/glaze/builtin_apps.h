/*
 * The built-in apps on the home screen, in order: X(label, icon, path,
 * status style, dock). SpringBoard's home screen and Settings → Icon
 * Buttons both list them; a RAM boot's romfs may hold only some. `dock`
 * puts an app in the toolbar until the user rearranges the home screen
 * (the sb.layout preference, servers/springboard/layout.h).
 */
#ifndef GLAZE_BUILTIN_APPS_H
#define GLAZE_BUILTIN_APPS_H

#include <glaze/sb.h>

#define GZ_BUILTIN_APPS(X) \
    X("Calendar", "icon_calendar.gzi", "bin/calendar", SB_STATUS_GRAY, false) \
    X("Photos", "icon_photos.gzi", "bin/photos", SB_STATUS_BLACK, false) \
    X("Clock", "icon_clock.gzi", "bin/clock", SB_STATUS_BLACK, false) \
    X("Calculator", "icon_calculator.gzi", "bin/calculator", SB_STATUS_BLACK, false) \
    X("Files", "icon_files.gzi", "bin/files", SB_STATUS_GRAY, false) \
    X("Notes", "icon_notes.gzi", "bin/notes", SB_STATUS_GRAY, true) \
    X("Settings", "icon_settings.gzi", "bin/settings", SB_STATUS_GRAY, true) \
    X("Browser", "icon_web.gzi", "bin/browser", SB_STATUS_GRAY, false) \
    X("Music", "icon_music.gzi", "bin/music", SB_STATUS_BLACK, false) \
    X("Videos", "icon_video.gzi", "bin/video", SB_STATUS_BLACK, false) \
    X("App Store", "icon_appstore.gzi", "bin/appstore", SB_STATUS_GRAY, false)

#endif
