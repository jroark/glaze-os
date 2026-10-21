#include "../apps/wince/ordinals.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char map[] = "1 CoreOnly\ncommctrl.dll 1 CommonOnly\n3d.dll 7 ThirdParty\n";
    char name[64];
    assert(ce_ordinal_find(map, sizeof(map) - 1, "COREDLL.dll", 1, name, sizeof(name)) && !strcmp(name, "CoreOnly"));
    assert(ce_ordinal_find(map, sizeof(map) - 1, "COMMCTRL.dll", 1, name, sizeof(name)) && !strcmp(name, "CommonOnly"));
    assert(ce_ordinal_find(map, sizeof(map) - 1, "3d.dll", 7, name, sizeof(name)) && !strcmp(name, "ThirdParty"));
    assert(!ce_ordinal_find(map, sizeof(map) - 1, "other.dll", 1, name, sizeof(name)));
    assert(!ce_ordinal_find(map, sizeof(map) - 1, "commctrl.dll", 7, name, sizeof(name)));
    assert(!ce_ordinal_find(map, sizeof(map) - 1, "coredll.dll", 1, name, 4));
    const char *bad[] = {"65536 Overflow\n", "1 NoNewline", "1 \n", "dll 1 Bad Name\n", "dll  BadOrdinal\n", "1 Name\0Bad\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!ce_ordinal_find(bad[i], strlen(bad[i]), "dll", 1, name, sizeof(name)));
    for (unsigned n = 0; n < 10; n++)
        assert(!ce_ordinal_find(map, n, "coredll.dll", 1, name, sizeof(name)));
    puts("wince-ordinals: ALL PASS");
    return 0;
}
