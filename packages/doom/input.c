/* SPDX-License-Identifier: GPL-3.0-or-later
 * Preserve physical releases across menu changes and latch short presses.
 * Text keeps arrival order instead of sorting ASCII codes in a state bitmap. */
#include "port.h"
#include "doomkeys.h"
#include <string.h>
static uint8_t held[256], pending[256], delivered[256], mapping[32];
static uint8_t text[64];
static unsigned read_at,write_at;
void doom_input_pulse(unsigned key) { if (key && key<256) pending[key]=1; }
void doom_input_text(unsigned key)
{
    unsigned next=(write_at+1)%sizeof text;
    if (!key || key>=256 || next==read_at) return;
    text[write_at]=key; write_at=next;
}
void doom_input_release(void)
{
    memset(held,0,sizeof held); memset(pending,0,sizeof pending); memset(mapping,0,sizeof mapping);
    read_at=write_at=0;
}
void doom_input_key(unsigned code,int down,int in_menu)
{
    if (code>=sizeof mapping) return;
    if (down) {
        if (mapping[code]) return;
        unsigned key=doom_map_key(code,in_menu);
        if (key) { mapping[code]=key; held[key]=1; pending[key]=1; }
    } else {
        unsigned key=mapping[code]; mapping[code]=0;
        held[key]=0;
        /* OK and a keyboard Enter can map to the same Doom key. */
        for (unsigned i=0;i<sizeof mapping;i++) if (mapping[i]==key && key) held[key]=1;
    }
}
void doom_input_run(int on) { held[KEY_RSHIFT]=!!on; }
int doom_input_next(int *pressed,unsigned char *key)
{
    for (unsigned i=1;i<256;i++) {
        if (pending[i] && !delivered[i]) {
            pending[i]=0; delivered[i]=1; *pressed=1; *key=i; return 1;
        }
        if (delivered[i]!=held[i]) {
            delivered[i]=held[i]; pending[i]=0; *pressed=held[i]; *key=i; return 1;
        }
    }
    if (read_at!=write_at) {
        unsigned k=text[read_at]; read_at=(read_at+1)%sizeof text;
        delivered[k]=1; *pressed=1; *key=k; return 1;
    }
    return 0;
}
