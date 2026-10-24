/* Exercise the real DOM/JS/loader together, with ASan/UBSan in host/Makefile. */
#include "../apps/browser/web.h"
#include "../apps/browser/script.h"
#include "../apps/browser/load.h"
#include "../apps/browser/frames.h"
#include "../apps/browser/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
static bool js(struct page *p, const char *code)
{
    return script_eval(p, code, (int)strlen(code), NULL);
}
static struct page *blank(const char *url)
{
    struct page *p = page_new(url);
    CHECK(p != NULL);
    if (p)
        page_set_html(p, "<p id='out'>before</p><button id='button' "
                         "onclick='this.textContent=\"clicked\";return false'>tap</button><form "
                         "id='form'><input id='field' name='q' value='old'></form>");
    return p;
}
static void engine(void)
{
    struct page *p = blank("http://one.test/index");
    CHECK(js(p, "function check(c){if(!c)throw Error('assertion');} "
                "check(6*7===42);check(Math.sqrt(81)===9);check(Math.abs(Math.sin(Math.PI/"
                "2)-1)<1e-12);check(JSON.parse('{\"x\":3}').x===3);"));
    CHECK(js(p, "var "
                "out=document.getElementById('out');check(out===document.querySelector('#out'));"
                "out.textContent='after';check(out.firstChild.nodeValue==='after');check(out."
                "parentNode===document.body);check(out.ownerDocument===document);var "
                "e=document.createElement('span');e.className='added';e.appendChild(document."
                "createTextNode('new'));out.appendChild(e);check(document.querySelectorAll('.added'"
                ").length===1);check(e.previousSibling.nodeType===3);e.style.color='red';check(e."
                "getAttribute('style').indexOf('color:red')>=0);out.removeChild(e);check(!e."
                "isConnected);out.innerHTML='<b>bold</"
                "b>';check(out.firstElementChild.tagName==='B');check(out.textContent==='bold');"
                "check(out.innerHTML==='<b>bold</b>');"));
    CHECK(js(
        p,
        "var "
        "field=document.getElementById('field');field.value='new';field.checked=true;check(field."
        "value==='new'&&field.checked);document.title='Title';check(document.title==='Title');var "
        "count=0;document.body.addEventListener('click',function(e){count++;check(e.target."
        "nodeType===3||e.target.id==='button');});"));
    struct node *button = dom_query(p, p->doc, "#button", NULL);
    CHECK(button != NULL);
    CHECK(!script_event(p, button, "click"));
    CHECK(js(
        p, "check(document.getElementById('button').textContent==='clicked');check(count===1);var "
           "done=false;Promise.resolve(42).then(function(x){done=x===42;});setTimeout(function(){"
           "document.getElementById('out').textContent='timer';},4);"));
    usleep(12000);
    script_tick(p);
    CHECK(js(p, "check(done);check(out.textContent==='timer');"));
    uint32_t start = plat_ms();
    CHECK(!js(p, "while(true){}"));
    CHECK(plat_ms() - start < 1000);
    CHECK(js(p, "check(2+2===4);"));
    CHECK(!js(p, "function recurse(){return recurse();}recurse();"));
    start = plat_ms();
    js(p, "/^(a+)+$/.test('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!');");
    CHECK(plat_ms() - start < 1000);
    CHECK(js(p, "check(3+3===6);"));
    start = plat_ms();
    CHECK(!js(p, "function forever(){Promise.resolve().then(forever);}forever();"));
    CHECK(plat_ms() - start < 1000);
    CHECK(js(p, "check(4+4===8);"));
    /* innerHTML scripts are inert; document.write inserted scripts are handled
       by the loader's document-order traversal. */
    CHECK(js(p, "out.innerHTML='<script>throw Error(\"inert\");<'+ '/script>';"));
    page_collect(p);
    CHECK(p->nscripts == 0);
    page_free(p);
}
static void policies(void)
{
    struct page *p = blank("https://one.test/index");
    p->csp = strdup("default-src 'self'; script-src 'self' 'nonce-abc'");
    CHECK(!script_allowed(p, NULL, NULL));
    CHECK(script_allowed(p, "https://one.test/a.js", NULL));
    CHECK(!script_allowed(p, "https://two.test/a.js", NULL));
    struct node *n = dom_create(p, N_ELEM, "script", NULL);
    dom_attr(p, n, "nonce", "abc");
    CHECK(script_allowed(p, NULL, n));
    CHECK(js(p, "var blocked=false;try{eval('1+1')}catch(e){blocked=true}if(!blocked)throw "
                "Error('eval escaped');[function(){},function*(){},async function(){},async "
                "function*(){}].forEach(function(f){var denied=false;try{f.constructor('return "
                "42')}catch(e){denied=true;}if(!denied)throw Error('constructor escaped');});"));
    CHECK(!page_resource_allowed(p, "file:///private"));
    CHECK(!page_resource_allowed(p, "res:start.html"));
    CHECK(frame_response_allowed(p, "https://one.test/frame", "SAMEORIGIN", NULL));
    CHECK(!frame_response_allowed(p, "https://two.test/frame", "SAMEORIGIN", NULL));
    CHECK(!frame_response_allowed(p, "https://one.test/frame", "DENY", NULL));
    CHECK(!frame_response_allowed(p, "https://one.test/frame", NULL, "frame-ancestors 'none'"));
    CHECK(frame_response_allowed(p, "https://one.test/frame", NULL, "frame-ancestors 'self'"));
    free(p->csp);
    p->csp = strdup("script-src 'unsafe-inline' 'nonce-abc' 'strict-dynamic'");
    CHECK(!script_allowed(p, NULL, NULL));
    CHECK(script_allowed(p, NULL, n));
    CHECK(script_allowed(p, "https://two.test/a.js", n));
    CHECK(!script_allowed(p, "https://one.test/a.js", NULL));
    page_free(p);
}
static void origins(void)
{
    struct page *p = blank("http://one.test/parent");
    struct node *n = dom_create(p, N_ELEM, "iframe", NULL);
    dom_attr(p, n, "src", "http://one.test/child");
    dom_attr(p, n, "id", "frame");
    dom_insert(p, p->body, n, NULL);
    page_collect(p);
    struct frame_slot *f = &p->frames[n->frame];
    f->page = blank("http://one.test/child");
    f->page->parent = p;
    f->page->host = f;
    CHECK(script_init(p));
    CHECK(script_init(f->page));
    CHECK(js(p, "var saved=document.getElementById('frame').contentDocument;var "
                "savedNode=saved.getElementById('out');if(!savedNode)throw Error('same origin "
                "denied');savedNode.textContent='parent edited';"));
    CHECK(js(f->page,
             "if(document.getElementById('out').textContent!=='parent edited')throw Error('child "
             "DOM');if(parent.document.getElementById('out').textContent!=='before')throw "
             "Error('parent DOM');"));
    frame_cancel(f);
    f->page = blank("http://two.test/foreign");
    f->page->parent = p;
    f->page->host = f;
    CHECK(script_init(f->page));
    CHECK(js(p, "if(document.getElementById('frame').contentDocument!==null)throw Error('foreign "
                "access');savedNode.textContent='stale';"));
    CHECK(js(f->page, "if(parent!==null)throw Error('foreign "
                      "parent');if(document.getElementById('out').textContent!=='before')throw "
                      "Error('stale handle wrote new DOM');"));
    page_free(p);
}
static void oom(void)
{
    struct page *p = blank("http://one.test/oom");
    CHECK(!js(p, "var leak=[];while(true){leak.push(new Array(4096).fill('xxxxxxxxxxxxxxxx'));}"));
    CHECK(p->script_disabled);
    CHECK(p->doc != NULL);
    page_style(p);
    page_layout(p, 240);
    CHECK(p->nitems > 0);
    page_free(p);
    p = blank("http://one.test/recovered");
    CHECK(js(p, "if(40+2!==42)throw Error('restart');"));
    page_free(p);
}
static struct page *fetch(struct load *ld, const char *url)
{
    load_begin(ld, url, NULL, NULL, false, 240);
    uint32_t start = plat_ms();
    while (plat_ms() - start < 5000) {
        int flags = load_step(ld);
        if (flags & LF_DONE)
            return ld->shown;
        usleep(1000);
    }
    CHECK(0 && "load timeout");
    return ld->shown;
}
static void transport(const char *base)
{
    char url[256];
    struct load ld = {0};
    snprintf(url, sizeof url, "%s/index", base);
    struct page *p = fetch(&ld, url);
    CHECK(p != NULL);
    if (!p)
        return;
    CHECK(js(
        p,
        "if(order.join(',')!=='inline,external,written,after,ready,load')throw Error('order "
        "'+order);if(document.getElementById('out').textContent!=='external')throw Error('external "
        "DOM');if(document.getElementById('frame').contentDocument.getElementById('child')."
        "textContent!=='child JS')throw Error('child "
        "scripts');if(document.getElementById('denied').contentDocument!==null)throw Error('XFO "
        "escaped');if(document.getElementById('sandbox').contentDocument!==null)throw "
        "Error('sandbox escaped');"));
    CHECK(frame_count_tree(p) <= PAGE_MAX_FRAMES);
    CHECK(p->nframes == 4);
    struct frame_slot *f = &p->frames[0];
    CHECK(f->page && f->page->width > 0);
    CHECK(f->page && f->page->content_h > f->h);
    struct ditem *hit = f->page ? page_hit(p, f->x + 8, f->y + 10) : NULL;
    CHECK(hit != NULL);
    CHECK(js(
        p, "var dynamicDone=false;var "
           "dynamic=document.createElement('iframe');dynamic.id='dynamic';dynamic.name='dynamic';"
           "dynamic.srcdoc='<p "
           "id=inline>inline</"
           "p>';dynamic.onload=function(){dynamicDone=true;};document.body.appendChild(dynamic);"));
    uint32_t dynamic_start = plat_ms();
    while (plat_ms() - dynamic_start < 1000) {
        load_step(&ld);
        usleep(1000);
    }
    CHECK(js(p, "if(!dynamicDone||frames.dynamic.document.getElementById('inline')===null)throw "
                "Error('dynamic frame');"));
    CHECK(page_navigate(p, "/next", "dynamic", NULL));
    dynamic_start = plat_ms();
    while (plat_ms() - dynamic_start < 1000) {
        load_step(&ld);
        usleep(1000);
    }
    CHECK(js(p, "if(dynamic.contentDocument.getElementById('next')===null)throw Error('srcdoc "
                "navigation');"));
    /* Child navigation replaces only that browsing context. */
    unsigned old = f->page ? f->page->serial : 0;
    CHECK(page_navigate(p, "/next", "child", NULL));
    uint32_t start = plat_ms();
    while (plat_ms() - start < 3000) {
        load_step(&ld);
        if (f->page && f->page->serial != old && f->page->load_event)
            break;
        usleep(1000);
    }
    CHECK(f->page && f->page->serial != old);
    CHECK(js(p, "if(document.getElementById('frame').contentDocument.getElementById('next')===null)"
                "throw Error('target navigation');"));
    struct page *old_page = p;
    snprintf(url, sizeof url, "%s/index", base);
    p = fetch(&ld, url);
    CHECK(old_page->script_disabled && !old_page->js);
    CHECK(p && js(p, "if(order[order.length-1]!=='load')throw Error('replacement scripts');"));
    page_free(old_page);
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
    memset(&ld, 0, sizeof ld);
    snprintf(url, sizeof url, "%s/frameset", base);
    p = fetch(&ld, url);
    CHECK(p && p->nitems == 2);
    CHECK(p && p->items[0].kind == DI_FRAME);
    CHECK(p && p->items[1].x >= 110);
    CHECK(p && p->content_h == 226);
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
    memset(&ld, 0, sizeof ld);
    snprintf(url, sizeof url, "%s/csp", base);
    p = fetch(&ld, url);
    CHECK(p && p->title && !strcmp(p->title, "external allowed"));
    CHECK(p && p->body);
    if (p)
        CHECK(js(p, "if(typeof forbidden!=='undefined')throw Error('CSP inline');"));
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
    memset(&ld, 0, sizeof ld);
    snprintf(url, sizeof url, "%s/limits", base);
    p = fetch(&ld, url);
    CHECK(p && frame_count_tree(p) <= PAGE_MAX_FRAMES);
    CHECK(p && frame_count_tree(p) > 2);
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
}
int main(int argc, char **argv)
{
    engine();
    policies();
    origins();
    oom();
    if (argc > 1)
        transport(argv[1]);
    script_shutdown();
    printf("browser scripts/frames: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
