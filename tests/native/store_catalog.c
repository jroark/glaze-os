#include "../../apps/appstore/catalog.h"
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
    assert(store_parse(&catalog, text, n) && catalog.count >= 9);
    assert(store_matches(&catalog.apps[0], "HELLO") && store_matches(&catalog.apps[0], "glaze") &&
           !store_matches(&catalog.apps[0], "not-an-app"));
    assert(!strcmp(catalog.apps[3].id,"minesweeper") && store_matches(&catalog.apps[3],"MINEFIELD"));
    assert(!strcmp(catalog.apps[4].id,"solitaire") && store_matches(&catalog.apps[4],"Klondike"));
    assert(!strcmp(catalog.apps[5].id,"taskmanager") && store_matches(&catalog.apps[5],"CPU"));
    assert(!strcmp(catalog.apps[6].id,"aim") && store_matches(&catalog.apps[6],"buddies"));
    assert(!strcmp(catalog.apps[7].id,"gmail") && store_matches(&catalog.apps[7],"IMAPS"));
    bool radio_found = false;
    for (int i = 0; i < catalog.count; i++)
        if (!strcmp(catalog.apps[i].id, "somafm")) {
            assert(store_matches(&catalog.apps[i], "radio") && store_matches(&catalog.apps[i], "Ethernet"));
            radio_found = true;
        }
    assert(radio_found);
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

    /* The OS update record (tools/mkota.py's os/catalog.txt). */
    static const char os_text[] = "GZOS1\n[os]\nversion=1.1\nbuild=abc\npackage=os/glaze-1.1.gzu\n"
        "size=4900000\nsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "spl_min=2\nnotes=Faster Notes.\n";
    struct store_os os;
    assert(store_os_parse(&os, os_text, strlen(os_text)));
    assert(!strcmp(os.version, "1.1") && !strcmp(os.package, "os/glaze-1.1.gzu") &&
           os.size == 4900000 && os.spl_min == 2 && !strcmp(os.notes, "Faster Notes."));
    char os_copy[sizeof(os_text)];
    for (size_t i = 6; i < strlen(os_text); i++) {
        /* Removing any required field's line makes it invalid. */
        if (os_text[i - 1] != '\n' || !strncmp(os_text + i, "build", 5) || !strncmp(os_text + i, "notes", 5))
            continue;
        const char *eol = strchr(os_text + i, '\n');
        size_t keep = i;
        memcpy(os_copy, os_text, keep);
        strcpy(os_copy + keep, eol + 1);
        assert(!store_os_parse(&os, os_copy, strlen(os_copy)));
    }
    strcpy(os_copy, os_text);
    strstr(os_copy, "size=")[5] = '9';      /* 9900000: too large */
    assert(!store_os_parse(&os, os_copy, strlen(os_copy)));
    strcpy(os_copy, os_text);
    strstr(os_copy, "package=")[8] = '/';   /* an absolute path */
    assert(!store_os_parse(&os, os_copy, strlen(os_copy)));
    assert(!store_os_parse(&os, "GZOS2\n", 6));
    puts("store catalog: PASS");
}
