"""Run the production maintenance companion with native service fixtures."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glaze/fs.h>
#include <glaze/pkg.h>
#include <glaze/ota.h>
static size_t fixture_strlcpy(char *d,const char *s,size_t n) { size_t len=strlen(s);if(n){size_t copy=len<n-1?len:n-1;memcpy(d,s,copy);d[copy]=0;}return len; }
#define strlcpy fixture_strlcpy
#define main companion_main
#define dlsym fixture_symbol
#include "packages/maintenance/main.c"
#undef main
static const char *scenario;
static unsigned tick, request;
int gz_ota_status(struct gz_ota_status *s) { memset(s,0,sizeof(*s)); s->flags=1; strcpy(s->version,"1.0"); return 0; }
int gz_power_status(struct gz_power_status *s) { memset(s,0,sizeof(*s)); s->flags=GZ_POWER_AC; if(!strcmp(scenario,"low-power"))s->flags=0; return 0; }
const void *gz_romfs_map(const char *p,uint32_t *size) { *size=1;return strcmp(p,"bin/calculator")?(const void *)1:NULL; }
uint32_t gz_time_ms(void) { return tick++; }
void gz_sleep_ms(uint32_t n) { tick+=n; }
void ui_init(int n) { (void)n; }
int ui_next_event(struct gz_input_event *e,int n) { (void)e;(void)n; return 0; }
int fs_statfs(struct fs_info *s) { memset(s,0,sizeof(*s)); s->total=6000000; return 0; }
int fs_read_file(const char *p,void *b,uint32_t n) {
    (void)p; char text[96];
    const char *action=request++ ? "quit" : (!strncmp(scenario,"ota",3) || !strcmp(scenario,"low-power") || !strcmp(scenario,"unsupported"))?"update":"install";
    if(request==1 && !strncmp(scenario,"uninstall:",10))action=scenario;
    snprintf(text,sizeof(text),"0123456789abcdef0123456789abcdef %s",action);
    assert(strlen(text)<n);strcpy(b,text); return strlen(text);
}
int fs_write_file(const char *p,const void *b,uint32_t n) { (void)p; fwrite(b,1,n,stdout);fflush(stdout);return n; }
int fs_remove(const char *p) { (void)p;return 0; }
int fs_rename(const char *a,const char *b) { (void)a;(void)b;return 0; }
int pkg_list(const char *p,void (*fn)(const char *,const struct pkg_info *,void *),void *arg) { (void)p;(void)fn;(void)arg;return 0; }
int pkg_read_info(const char *p,struct pkg_info *i) { (void)p;memset(i,0,sizeof(*i));strcpy(i->id,"test");strcpy(i->version,"1.1");return 0; }
int pkg_find(const char *id,char bundle[PKG_BUNDLE_MAX]) { (void)id;strcpy(bundle,!strcmp(scenario,"card")?"/Card/Applications/test.app":"/Applications/test.app"); return !strcmp(scenario,"new")?GZ_ENOENT:0; }
int pkg_bundle_info(const char *p,struct pkg_info *i) { (void)p;memset(i,0,sizeof(*i));strcpy(i->id,"test");strcpy(i->version,!strcmp(scenario,"skip")?"1.2":"1.0");return 0; }
int pkg_install(const char *p,const char *root,char bundle[PKG_BUNDLE_MAX]) { (void)p;(void)bundle;printf("INSTALL:%s\n",root);return !strcmp(scenario,"full")?GZ_ENOSPC:0; }
int pkg_uninstall(const char *p) { printf("REMOVE:%s\n",p);return 0; }
static int begin_fixture(const char *p) { assert(!strcmp(p,"/Library/Updates/installer.gzu"));puts("BEGIN");return !strcmp(scenario,"ota-bad")?GZ_EINVAL:8192; }
static int step_fixture(void) { puts("STEP");return 8192; }
static int arm_fixture(void) { puts("ARM");fflush(stdout);exit(0); }
static int cancel_fixture(void) { puts("CANCEL");return 0; }
void *fixture_symbol(void *h,const char *s) { (void)h;if(!strcmp(scenario,"unsupported") || !strcmp(scenario,"old-app"))return NULL;
    if(!strcmp(s,"fs_ota_begin"))return begin_fixture;
    if(!strcmp(s,"fs_ota_step"))return step_fixture;
    if(!strcmp(s,"fs_ota_arm"))return arm_fixture;
    return cancel_fixture;
}
int main(int argc,char **argv) { assert(argc==2);scenario=argv[1];return companion_main(); }
'''

class Companion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        folder = Path(cls.tmp.name)
        (folder / 'glaze').mkdir()
        (folder / 'glaze/sys.h').write_text('''#include <glaze/abi.h>
#include <glaze/ota.h>
int gz_power_status(struct gz_power_status *);
int gz_ota_status(struct gz_ota_status *);
uint32_t gz_time_ms(void);
void gz_sleep_ms(uint32_t);
const void *gz_romfs_map(const char *,uint32_t *);
''')
        (folder / 'dlfcn.h').write_text('void *dlsym(void *, const char *);\n')
        (folder / 'glaze/ui.h').write_text('''#include <glaze/sb.h>
#define gfx_fill(...) ((void)0)
#define ui_draw_navbar(...) ((void)0)
#define gfx_text_center(a,b,c,d,e,message,f,g) ((void)(message))
#define ui_invalidate_all() ((void)0)
#define ui_present() ((void)0)
void ui_init(int);
int ui_next_event(struct gz_input_event *,int);
''')
        (folder / 'test.c').write_text(HARNESS)
        cls.binary = folder / 'test'
        subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(folder), '-I'+str(ROOT / 'lib/include'), '-I'+str(ROOT), str(folder / 'test.c'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, scenario):
        lines = subprocess.check_output([str(self.binary), scenario], text=True).splitlines()
        replies = [json.loads(line) for line in lines if line.startswith('{')]
        return lines, replies

    def test_new_and_card_installs(self):
        for case, location in [('new','INSTALL:'), ('card','INSTALL:/Card')]:
            lines, replies = self.run_case(case)
            self.assertIn(location,lines)
            self.assertEqual(replies[0]['code'],0)

    def test_skip_newer(self):
        lines, replies = self.run_case('skip')
        self.assertFalse(any(s.startswith('INSTALL:') for s in lines))
        self.assertEqual(replies[0]['code'],1)

    def test_old_os_app_install(self):
        lines, replies = self.run_case('old-app')
        self.assertIn('INSTALL:', lines)
        self.assertEqual(replies[0]['code'], 0)

    def test_failed_install(self):
        _, replies = self.run_case('full')
        self.assertLess(replies[0]['code'],0)

    def test_ota_preconditions(self):
        for case in ('low-power','unsupported','ota-bad'):
            lines, replies = self.run_case(case)
            self.assertNotIn('ARM',lines)
            self.assertLess(replies[0]['code'],0)

    def test_ota_verifies_before_arm(self):
        lines, replies = self.run_case('ota-good')
        self.assertLess(lines.index('BEGIN'), lines.index('STEP'))
        self.assertLess(lines.index('STEP'), lines.index('ARM'))
        self.assertEqual(replies[0]['phase'],'arming')

    def test_inventory_reports_builtin_and_firmware_presence(self):
        _, replies = self.run_case('new')
        self.assertEqual(replies[0]['protocol'], 2)
        self.assertIn('files', replies[0]['builtins'])
        self.assertNotIn('calculator', replies[0]['builtins'])
        self.assertTrue(replies[0]['wl54cf'])

    def test_uninstall_validated_bundle(self):
        lines, replies = self.run_case('uninstall:test')
        self.assertIn('REMOVE:/Applications/test.app', lines)
        self.assertEqual(replies[0]['code'], 0)

    def test_uninstall_rejects_paths_and_system_ids(self):
        for ident in ('files', 'settings', 'net', 'maintenance', '../test', '/Applications/test.app', 'Test'):
            lines, replies = self.run_case('uninstall:' + ident)
            self.assertLess(replies[0]['code'], 0)
            self.assertNotIn('REMOVE:/Applications/test.app', lines)

if __name__ == '__main__':
    unittest.main()
