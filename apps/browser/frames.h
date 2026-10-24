#ifndef BROWSER_FRAMES_H
#define BROWSER_FRAMES_H
#include "web.h"
bool frameset_layout(struct page *p, int width);
void frame_geometry(struct frame_slot *f, int x, int y, int w, int h);
bool node_connected(const struct node *n);
int frame_count_tree(struct page *p);
void frame_cancel(struct frame_slot *f);
#endif
