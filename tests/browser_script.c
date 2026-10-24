/* Exercise the real DOM/JS/loader together, with ASan/UBSan in host/Makefile. */
#include "../apps/browser/web.h"
#include "../apps/browser/script.h"
#include "../apps/browser/load.h"
#include "../apps/browser/frames.h"
#include "../apps/browser/plat.h"
#include "../apps/browser/identity.h"
#include "../apps/browser/net.h"
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
static void noscript(void)
{
    const char *fallback = "<meta http-equiv=refresh content='0;url=/enablejs'>"
        "<base href='https://wrong.test/'><link rel=stylesheet href='/fallback.css'>"
        "<style>body{color:red}</style><img src='/fallback.png'>"
        "<script src='/fallback.js'></script><b id=fallback>&amp;</b>";
    for (int enabled = 0; enabled < 2; enabled++) {
        for (int head = 0; head < 2; head++) {
            struct page *p = page_new("http://one.test/noscript");
            CHECK(p != NULL);
            if (!p)
                continue;
            p->scripting_enabled = enabled;
            char html[1024];
            snprintf(html, sizeof html, "<html><head>%s<noscript id=ns>%s</NoScRiPt>"
                     "<script>var neighbor=true;</script></head><body></body></html>",
                     head ? "" : "</head><body>", fallback);
            /* Exercise both the fetched-document and generated-HTML paths. */
            if (head)
                page_set_document(p, (const uint8_t *)html, (int)strlen(html), "text/html");
            else
                page_set_html(p, html);
            int delay = -1;
            const char *refresh = page_refresh(p, &delay);
            struct node *ns = dom_query(p, p->doc, "#ns", NULL);
            CHECK(ns != NULL);
            if (enabled) {
                CHECK(!refresh);
                CHECK(!strcmp(p->base, p->url));
                CHECK(p->nsheets == 0 && p->nimgs == 0 && p->nscripts == 1);
                CHECK(!dom_query(p, p->doc, "#fallback", NULL));
                CHECK(ns && ns->first && ns->first->type == N_TEXT && !ns->first->next);
                CHECK(ns && !strcmp(node_text(&p->arena, ns), fallback));
                char *serialized = ns ? dom_serialize(p, ns, false) : NULL;
                CHECK(serialized && !strcmp(serialized, fallback));
                free(serialized);
                CHECK(js(p, "document.title='neighbor ran';"));
                CHECK(p->title && !strcmp(p->title, "neighbor ran"));
            } else {
                CHECK(refresh && !strcmp(refresh, "/enablejs") && delay == 0);
                CHECK(dom_query(p, p->doc, "#fallback", NULL));
                CHECK(!script_init(p));
            }
            page_style(p);
            if (!head && ns && ns->st)
                CHECK((ns->st->display == D_NONE) == !!enabled);
            page_free(p);
        }
    }
    struct page *p = blank("http://one.test/fragments");
    CHECK(js(p, "var out=document.getElementById('out');"
                "out.innerHTML='<noscript id=ns><meta http-equiv=refresh "
                "content=\"0;url=/bad\"><b id=hidden>fallback</b></noscript>';"
                "var ns=document.getElementById('ns');"
                "if(document.getElementById('hidden'))throw Error('fragment fallback');"
                "ns.innerHTML='<meta http-equiv=refresh content=\"0;url=/bad\">&amp;';"
                "if(ns.firstChild.nodeType!==3||ns.childNodes.length!==1||"
                "ns.textContent!==ns.innerHTML)throw Error('noscript context');"));
    page_collect(p);
    int delay;
    CHECK(!page_refresh(p, &delay));
    page_free(p);
    p = page_new("http://one.test/slash");
    page_set_html(p, "<noscript/><meta http-equiv=refresh content='0;url=/bad'></noscript>"
                     "<meta http-equiv=refresh content='3;url=/normal'>");
    const char *refresh = page_refresh(p, &delay);
    CHECK(refresh && !strcmp(refresh, "/normal") && delay == 3);
    page_free(p);
}
static void engine(void)
{
    struct page *p = blank("http://one.test/index");
    CHECK(js(p, "function check(c){if(!c)throw Error('assertion');} "
                "check(6*7===42);check(Math.sqrt(81)===9);check(Math.abs(Math.sin(Math.PI/"
                "2)-1)<1e-12);check(JSON.parse('{\"x\":3}').x===3);"));
    /* Speculative arrow-parameter scanning must retain captured bindings. */
    CHECK(js(p, "function capture(U,H){true?(H=function(){return U[1]},U=[0,42]):null;return H();}"
                "check(capture()===42);"
                "function captureLocal(){var x,f;true?(f=function(){return x},x=42):null;return f;}"
                "check(captureLocal()()===42);"
                "function captureFalse(x,f){false?null:(f=function(){return x},x=42);return f;}"
                "check(captureFalse()()===42);"
                "var arrow=(f=function(){return x},x=42)=>f();check(arrow()===42);"
                "function sourceExample(){return 42;}check(sourceExample.toString().indexOf('return 42')>=0);"));
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
    p->ready_event = true; /* Exercise the interactive execution budget. */
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
static void navigator_identity(void)
{
    struct page *p = blank("http://one.test/navigator");
    CHECK(js(p, "function check(c){if(!c)throw Error('navigator assertion');}"
                "check(navigator===window.navigator&&navigator===clientInformation);"
                "check(navigator instanceof Navigator);"
                "check(Object.prototype.toString.call(navigator)==='[object Navigator]');"
                "check(navigator.userAgent==='" BROWSER_USER_AGENT "');"
                "check(navigator.appVersion===navigator.userAgent.slice(8));"
                "check(navigator.platform==='BE-300'&&navigator.appName==='Netscape');"
                "check(navigator.language==='" BROWSER_LANGUAGE "');"
                "check(navigator.languages.length===1&&navigator.languages[0]===navigator.language);"
                "check(Object.isFrozen(navigator.languages));"
                "check(navigator.cookieEnabled&&navigator.hardwareConcurrency===1&&navigator.maxTouchPoints===1);"));
    CHECK(js(p, "'use strict';var original=navigator,ua=navigator.userAgent;"
                "var denied=false;try{navigator.userAgent='other';}catch(e){denied=e instanceof TypeError;}"
                "check(denied&&navigator.userAgent===ua);"
                "denied=false;try{window.navigator={};}catch(e){denied=e instanceof TypeError;}"
                "check(denied&&navigator===original);"
                "denied=false;try{new Navigator();}catch(e){denied=e instanceof TypeError;}check(denied);"));
    /* Google's failing bare identifier read, followed by its feature check. */
    CHECK(js(p, "navigator||(window.navigator={});var calls=0;"
                "check(typeof navigator.sendBeacon==='undefined');"
                "if(typeof navigator.sendBeacon!=='function')navigator.sendBeacon=function(){calls++;return false;};"
                "check(navigator.sendBeacon('/collect','')===false&&calls===1);"));
    page_free(p);
    p = blank("http://one.test/fresh-navigator");
    CHECK(js(p, "if(typeof navigator.sendBeacon!=='undefined')throw Error('navigator leaked across navigation');"));
    page_free(p);
}
static void web_platform(void)
{
    struct page *p = blank("https://cookies.test/a/page?q=one#two");
    CHECK(js(p, "function check(c){if(!c)throw Error('platform assertion');}"
                "check(performance===window.performance&&performance.now()>=0);"
                "check(Math.abs(Date.now()-performance.timeOrigin-performance.now())<2000);"
                "check(performance.toJSON().timeOrigin===performance.timeOrigin);var before=performance.now();"
                "check(innerWidth===320&&innerHeight>0&&outerWidth===screen.width);"
                "check(screen.colorDepth===16&&screen.pixelDepth===16&&devicePixelRatio===0.75);"
                "check(location.search==='?q=one'&&location.hash==='#two'&&location.pathname==='/a/page');"
                "check(location.protocol==='https:'&&location.hostname==='cookies.test'&&location.port==='');"
                "check(location.origin==='https://cookies.test');"
                "check(btoa('hello')==='aGVsbG8='&&atob('aGVsbG8=')==='hello');"
                "check(atob(' Y Q = = \\n')==='a'&&atob('YQ')==='a');"
                "var bytes='';for(var i=0;i<256;i++)bytes+=String.fromCharCode(i);check(atob(btoa(bytes))===bytes);"
                "['a','YQ=','====','Y!Q=','YQ\\v'].forEach(function(v){var bad=false;try{atob(v)}catch(e){bad=e.name==='InvalidCharacterError'}check(bad)});"
                "var bad=false;try{btoa('\\u0100')}catch(e){bad=e.name==='InvalidCharacterError'}check(bad);"
                "document.cookie='visible=value; Path=/; Secure';check(document.cookie.indexOf('visible=value')>=0);"
                "document.cookie='gone=value';document.cookie='gone=; Expires=Thu, 01 Jan 1970 00:00:00 GMT';"
                "check(document.cookie.indexOf('gone=')<0);"
                "document.cookie='kept=yes; Max-Age=60; Expires=Thu, 01 Jan 1970 00:00:00 GMT';"
                "check(document.cookie.indexOf('kept=yes')>=0);"
                "document.cookie='bad=no; Domain=other.test';check(document.cookie.indexOf('bad=')<0);"
                "document.cookie='hidden=no; HttpOnly';check(document.cookie.indexOf('hidden=')<0);"
                "document.cookie='__Secure-bad=no';document.cookie='__Host-bad=no; Secure';"
                "check(document.cookie.indexOf('__Secure-bad=')<0&&document.cookie.indexOf('__Host-bad=')<0);"
                "document.cookie='scoped=no; Path=/other';check(document.cookie.indexOf('scoped=')<0);"
                "document.cookie='relative=yes; Path= ';check(document.cookie.indexOf('relative=yes')>=0);"));
    usleep(12000);
    CHECK(js(p, "check(performance.now()>=before+2);"));
    char *cookies = ld_document_cookie("http://cookies.test/a/page");
    CHECK(cookies && !strstr(cookies, "visible="));
    free(cookies);
    p->opaque_origin = true;
    CHECK(js(p, "check(document.cookie==='');document.cookie='opaque=no';check(document.cookie==='');"));
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
    CHECK(js(p, "var childNav=document.getElementById('frame').contentWindow.navigator;"
                "if(childNav===navigator||childNav.userAgent!==navigator.userAgent)throw Error('navigator realm');"
                "childNav.testMarker='child';if(navigator.testMarker!==undefined)throw Error('navigator shared');"));
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
    CHECK(js(p, "document.getElementById('out').innerHTML='<noscript id=ns><meta http-equiv=refresh "
                "content=\"0;url=/bad\"></noscript>';"));
    CHECK(!js(p, "var leak=[];while(true){leak.push(new Array(4096).fill('xxxxxxxxxxxxxxxx'));}"));
    CHECK(p->script_disabled);
    CHECK(p->scripting_enabled);
    int delay;
    CHECK(!page_refresh(p, &delay));
    /* Later fragment parsing must retain the original document mode too. */
    CHECK(dom_content(p, p->body, "<noscript id=late><meta http-equiv=refresh "
                      "content='0;url=/bad'></noscript><p>still readable</p>", true));
    page_collect(p);
    CHECK(!page_refresh(p, &delay));
    CHECK(p->doc != NULL);
    page_style(p);
    struct node *late = dom_query(p, p->doc, "#late", NULL);
    CHECK(late && late->st && late->st->display == D_NONE);
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
static void noscript_transport(const char *base)
{
    char url[256];
    struct load ld = {0};
    snprintf(url, sizeof url, "%s/noscript", base);
    struct page *p = fetch(&ld, url);
    CHECK(p != NULL);
    if (p) {
        int delay;
        CHECK(p->title && !strcmp(p->title, "neighbor ran"));
        CHECK(!page_refresh(p, &delay));
        CHECK(!strcmp(p->base, url));
        CHECK(p->nsheets == 0 && p->nimgs == 0 && p->nscripts == 1);
        CHECK(p->nframes == 2);
        for (int i = 0; i < p->nframes; i++) {
            struct page *child = p->frames[i].page;
            CHECK(child && !child->scripting_enabled && child->script_disabled && !child->js);
            if (!child)
                continue;
            struct node *fallback = dom_query(child, child->doc, "#fallback", NULL);
            CHECK(fallback && fallback->st && fallback->st->display != D_NONE);
            const char *refresh = page_refresh(child, &delay);
            CHECK(refresh && !strcmp(refresh, i == 0 ? "/local" : "/remote") && delay == 0);
        }
    }
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
}
static void navigator_transport(const char *base)
{
    char url[256];
    struct load ld = {0};
    snprintf(url, sizeof url, "%s/navigator", base);
    struct page *p = fetch(&ld, url);
    CHECK(p && p->title && !strcmp(p->title, "navigator matches headers"));
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
}
static void platform_transport(const char *base)
{
    char url[256];
    struct load ld = {0};
    snprintf(url, sizeof url, "%s/platform", base);
    struct page *p = fetch(&ld, url);
    CHECK(p != NULL);
    if (p) {
        CHECK(js(p, "if(document.cookie.indexOf('httpOnly=')>=0)throw Error('HttpOnly exposed');"
                    "document.cookie='httpOnly=overwritten; Path=/';"
                    "document.cookie='scriptCookie=sent; Path=/';"
                    "if(document.getElementById('cookie-child').contentDocument.title!=='inherited cookie')throw Error('inherited cookie');"
                    "if(!imageLoaded||!imageFailed||!pixel.complete||pixel.naturalWidth!==1)throw Error('detached images');"
                    "if(document.querySelector('img'))throw Error('detached image attached');"
                    "var ping=new Image();ping.src='/cookie-check';"));
        uint32_t start = plat_ms();
        while (plat_ms() - start < 2000) {
            load_step(&ld);
            usleep(1000);
        }
    }
    load_stop(&ld);
    load_forget(&ld, p);
    page_free(p);
    free(ld.url);
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
    noscript();
    engine();
    navigator_identity();
    web_platform();
    policies();
    origins();
    oom();
    if (argc > 1) {
        navigator_transport(argv[1]);
        platform_transport(argv[1]);
        noscript_transport(argv[1]);
        transport(argv[1]);
    }
    script_shutdown();
    printf("browser scripts/frames: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
