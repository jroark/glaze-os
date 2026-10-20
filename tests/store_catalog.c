#include "../apps/appstore/catalog.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct store_catalog catalog;
int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(store_version_ok("1.20.3") && !store_version_ok("1..2") && !store_version_ok("1.9999999") &&
           !store_version_ok("v1"));
    assert(store_version_cmp("1.10", "1.9") > 0 && store_version_cmp("1.0", "1") == 0 &&
           store_version_cmp("2", "2.0.1") < 0);
    assert(!store_path_ok("//evil/app") && !store_path_ok("a/../app") && !store_path_ok("https://evil") &&
           !store_path_ok("a%2fapp"));
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *text = calloc(1, n * 2 + 1024);
    assert(text);
    assert(fread(text, 1, n, f) == (size_t)n);
    fclose(f);
    assert(store_parse(&catalog, text, n) && catalog.count == 7);
    assert(store_matches(&catalog.apps[0], "HELLO") && store_matches(&catalog.apps[0], "glaze") &&
           !store_matches(&catalog.apps[0], "not-an-app"));
    assert(!strcmp(catalog.apps[3].id,"minesweeper") && store_matches(&catalog.apps[3],"MINEFIELD"));
    assert(!strcmp(catalog.apps[4].id,"solitaire") && store_matches(&catalog.apps[4],"Klondike"));
    assert(!strcmp(catalog.apps[5].id,"taskmanager") && store_matches(&catalog.apps[5],"CPU"));
    assert(!strcmp(catalog.apps[6].id,"aim") && store_matches(&catalog.apps[6],"buddies"));
    for (long i = 0; i < n; i++) {
        char c = text[i];
        text[i] = 0;
        assert(!store_parse(&catalog, text, n));
        text[i] = c;
    }
    /* Cuts that end a complete record may legitimately be smaller catalogs. */
    char *hash = strstr(text, "sha256=");
    assert(hash);
    char old = hash[7];
    hash[7] = 'z';
    assert(!store_parse(&catalog, text, n));
    hash[7] = old;
    char *size = strstr(text, "size=");
    assert(size);
    old = size[5];
    size[5] = '-';
    assert(!store_parse(&catalog, text, n));
    size[5] = old;
    assert(!store_parse(&catalog, "GZSTORE2\n", 9));
    char *app = strstr(text, "[app]");
    assert(app);
    size_t extra = n - (app - text);
    memcpy(text + n, app, extra);
    assert(!store_parse(&catalog, text, n + extra));
    assert(store_parse(&catalog, "GZSTORE1\n", 9) && !catalog.count);
    free(text);
    puts("store catalog: PASS");
}
