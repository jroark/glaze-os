/* Per-document asynchronous ES-module graph. No synchronous network I/O in
 * Jerry's resolver: all dependencies are fetched and parsed before linking. */
enum { M_WAIT, M_SOURCE, M_PARSED, M_ERROR };
static int module_find(struct runtime *s,const char *url)
{ for(int i=0;i<s->nmodules;i++)if(!strcmp(s->modules[i].url,url))return i;return -1; }
static int module_add(struct runtime *s,const char *url,const struct node *policy)
{
    int found=module_find(s,url);if(found>=0)return found;
    if(s->nmodules>=WEB_MODULES || !script_allowed(s->page,url,policy))return -1;
    char *copy=strdup(url);if(!copy)return -1;
    int i=s->nmodules++;s->modules[i].url=copy;s->modules[i].value=jerry_undefined();s->modules[i].policy=policy;return i;
}
static char *module_url(const char *base,const char *spec)
{
    /* Import maps/bare package specifiers are intentionally unsupported. */
    if(!spec || (strncmp(spec,"./",2)&&strncmp(spec,"../",3)&&*spec!='/'&&!strchr(spec,':')))return NULL;
    return temporary(url_resolve(base,spec));
}
static jerry_value_t module_user(struct runtime *s,const char *url,struct node *policy)
{
    jerry_value_t user=jerry_object();put_value(user,"serial",jerry_number(s->page->serial));put_string(user,"url",url);put_value(user,"policy",wrap(policy));return user;
}
static void module_schedule(struct runtime *s)
{
    for(int i=0;i<s->nmodules;i++){
        struct module_entry *m=&s->modules[i];
        if(m->state!=M_WAIT||m->fetch||!ld_can_start())continue;
        m->fetch=script_module_fetch(s->page,m->url,m->policy);
        if(m->fetch<=0){m->fetch=0;m->state=M_ERROR;}
    }
}
static void module_parse(struct runtime *s,struct module_entry *m)
{
    jerry_parse_options_t opts={.options=JERRY_PARSE_MODULE|JERRY_PARSE_HAS_SOURCE_NAME|JERRY_PARSE_HAS_USER_VALUE};
    opts.source_name=utf8_value(m->base?m->base:m->url);opts.user_value=module_user(s,m->base?m->base:m->url,(struct node *)m->policy);
    m->value=jerry_parse((const jerry_char_t *)m->source,(size_t)m->len,&opts);
    jerry_value_free(opts.source_name);jerry_value_free(opts.user_value);
    free(m->source);m->source=NULL;
    if(jerry_value_is_exception(m->value)){report(m->value);m->value=jerry_undefined();m->state=M_ERROR;return;}
    m->state=M_PARSED;
    size_t count=jerry_module_request_count(m->value);
    if(count>WEB_MODULES){m->state=M_ERROR;return;}
    for(size_t i=0;i<count;i++){
        int base=ntemps;
        jerry_value_t req=jerry_module_request(m->value,i);
        char *spec=utf8_arg(req),*url=module_url(m->base?m->base:m->url,spec);jerry_value_free(req);
        if(!url||module_add(s,url,m->policy)<0)m->state=M_ERROR;
        while(ntemps>base)free(temps[--ntemps]);
        if(m->state==M_ERROR)break;
    }
}
static jerry_value_t module_resolve(jerry_value_t spec,jerry_value_t referrer,void *arg)
{
    struct runtime *s=arg;
    for(int i=0;i<s->nmodules;i++)if(s->modules[i].value==referrer){
        int base=ntemps;
        char *text=utf8_arg(spec),*url=module_url(s->modules[i].base?s->modules[i].base:s->modules[i].url,text);
        int found=url?module_find(s,url):-1;
        while(ntemps>base)free(temps[--ntemps]);
        if(found>=0 && s->modules[found].state==M_PARSED)return jerry_value_copy(s->modules[found].value);
        break;
    }
    return jerry_throw_sz(JERRY_ERROR_TYPE,"Module dependency unavailable");
}
/* Only the root's reachable dependencies delay it; an unrelated dynamic
 * import's network failure must not stop other module graphs. */
static int module_ready(struct runtime *s,int index,uint32_t *seen)
{
    uint32_t bit=1u<<index;if(*seen&bit)return 1;*seen|=bit;
    struct module_entry *m=&s->modules[index];
    if(m->state==M_ERROR)return -1;
    if(m->state!=M_PARSED)return 0;
    if(jerry_module_state(m->value)==JERRY_MODULE_STATE_EVALUATED)return 1;
    if(jerry_module_state(m->value)==JERRY_MODULE_STATE_ERROR)return -1;
    size_t count=jerry_module_request_count(m->value);int ready=1;
    for(size_t i=0;i<count;i++){
        int base=ntemps;
        jerry_value_t req=jerry_module_request(m->value,i);
        char *spec=utf8_arg(req),*url=module_url(m->base?m->base:m->url,spec);jerry_value_free(req);
        int dep=url?module_find(s,url):-1;
        while(ntemps>base)free(temps[--ntemps]);
        int r=dep<0?-1:module_ready(s,dep,seen);
        if(r<0)return -1;
        if(!r)ready=0;
    }
    return ready;
}
static int module_run(struct runtime *s,int index)
{
    for(int i=0;i<s->nmodules;i++)if(s->modules[i].state==M_SOURCE)module_parse(s,&s->modules[i]);
    module_schedule(s);
    uint32_t seen=0;int ready=module_ready(s,index,&seen);if(ready<=0)return ready;
    struct module_entry *m=&s->modules[index];jerry_module_state_t state=jerry_module_state(m->value);
    if(state==JERRY_MODULE_STATE_EVALUATED)return 1;
    if(state==JERRY_MODULE_STATE_ERROR)return -1;
    if(state==JERRY_MODULE_STATE_UNLINKED){
        jerry_value_t result=jerry_module_link(m->value,module_resolve,s);
        bool ok=!jerry_value_is_exception(result);report(result);if(!ok){m->state=M_ERROR;return -1;}
    }
    jerry_value_t result=jerry_module_evaluate(m->value);
    bool ok=!jerry_value_is_exception(result);report(result);
    if(!ok)m->state=M_ERROR;
    return ok?1:-1;
}
static void module_meta(jerry_value_t module,jerry_value_t meta,void *arg)
{
    (void)arg;
    for(int p=0;p<JS_PAGES;p++)if(pages[p])for(int i=0;i<pages[p]->nmodules;i++)
        if(pages[p]->modules[i].value==module){
            struct module_entry *m=&pages[p]->modules[i];
            put_string(meta,"url",m->root&&!node_attr(m->root,"src")?pages[p]->page->url:m->base?m->base:m->url);return;
        }
}
static jerry_value_t module_import(jerry_value_t spec,jerry_value_t user,void *arg)
{
    (void)arg;
    if(!jerry_value_is_object(user))return jerry_throw_sz(JERRY_ERROR_TYPE,"No import context");
    jerry_value_t serial=jerry_object_get_sz(user,"serial");
    struct runtime *s=jerry_value_is_number(serial)?by_serial((uintptr_t)jerry_value_as_number(serial)):NULL;
    jerry_value_free(serial);
    if(!s)return jerry_throw_sz(JERRY_ERROR_TYPE,"Import context retired");
    char *base=field_string(user,"url"),*text=utf8_arg(spec),*url=base?module_url(base,text):NULL;
    jerry_value_t policy=jerry_object_get_sz(user,"policy");
    struct node *node=node_arg(policy);jerry_value_free(policy);
    int index=url?module_add(s,url,node):-1;
    if(index<0)return jerry_throw_sz(JERRY_ERROR_TYPE,"Module blocked or graph limit reached");
    for(int i=0;i<8;i++)if(!s->imports[i].used){
        s->imports[i].used=true;s->imports[i].index=index;s->imports[i].promise=jerry_promise();
        return jerry_value_copy(s->imports[i].promise);
    }
    return jerry_throw_sz(JERRY_ERROR_TYPE,"Too many pending imports");
}
static void module_tick(struct runtime *s)
{
    for(int i=0;i<8;i++)if(s->imports[i].used){
        int ready=module_run(s,s->imports[i].index);if(!ready)continue;
        jerry_value_t value=ready>0?jerry_module_namespace(s->modules[s->imports[i].index].value):
            jerry_error_sz(JERRY_ERROR_TYPE,"Module loading failed");
        jerry_value_t result=ready>0?jerry_promise_resolve(s->imports[i].promise,value):jerry_promise_reject(s->imports[i].promise,value);
        jerry_value_free(result);jerry_value_free(value);jerry_value_free(s->imports[i].promise);s->imports[i].used=false;
    }
}
static void modules_free(struct runtime *s,bool engine)
{
    for(int i=0;i<s->nmodules;i++){
        ld_cancel(s->modules[i].fetch);free(s->modules[i].url);free(s->modules[i].source);free(s->modules[i].base);
        if(engine)jerry_value_free(s->modules[i].value);
    }
    if(engine)for(int i=0;i<8;i++)if(s->imports[i].used)jerry_value_free(s->imports[i].promise);
}
bool script_response(struct page *p,struct fetch_result *r)
{
    if(!p || !p->js)return false;
    struct runtime *s=p->js;
    for(int i=0;i<WEB_REQUESTS;i++)if(s->requests[i].id==r->id){
        s->requests[i].result=*r;s->requests[i].done=true;memset(r,0,sizeof *r);return true;
    }
    for(int i=0;i<s->nmodules;i++)if(s->modules[i].fetch==r->id){
        struct module_entry *m=&s->modules[i];m->fetch=0;m->state=M_ERROR;
        bool mime=script_module_mime(r->ctype);
        if(!r->err&&r->status>=200&&r->status<300&&mime&&script_allowed(p,r->url,m->policy)&&
            s->module_bytes+r->len<=256*1024){
            m->source=malloc((size_t)r->len+1);
            if(m->source){memcpy(m->source,r->data,(size_t)r->len);m->source[r->len]=0;m->len=r->len;free(m->base);m->base=strdup(r->url);m->state=M_SOURCE;s->module_bytes+=r->len;}
        }
        return true;
    }
    return false;
}
int script_module(struct page *p,const char *source,int len,struct node *current,const char *url)
{
    if(!script_init(p)||!source||len<0||len>131072)return -1;
    js_failure_guard=true;
    if(setjmp(js_failure_boundary)){failed();return -1;}
    budget(__builtin_frame_address(0),!p->ready_event);
    struct runtime *s=p->js;jerry_value_t old=jerry_set_realm(s->realm);
    int index=-1;
    if(url)index=module_find(s,url);
    else for(int i=0;i<s->nmodules;i++)if(s->modules[i].root==current){index=i;break;}
    if(index<0){
        char *key=temporary(malloc(4096));
        if(key){snprintf(key,4096,"%s#glaze-inline-%d",p->base,s->nmodules);index=module_add(s,url?url:key,current);}
        if(index>=0){
            struct module_entry *m=&s->modules[index];m->root=current;
            if(s->module_bytes+len>256*1024)m->state=M_ERROR;
            else {m->source=malloc((size_t)len+1);if(!m->source)m->state=M_ERROR;
                else{memcpy(m->source,source,(size_t)len);m->source[len]=0;m->len=len;m->state=M_SOURCE;s->module_bytes+=len;}}
        }
    }
    int ready=index<0?-1:module_run(s,index);
    report(jerry_run_jobs());(void)jerry_set_realm(old);clean_temps();js_failure_guard=running=false;return ready;
}
int script_module_fetch(struct page *p,const char *url,const struct node *policy)
{
    /* Called outside the VM; use independent bounded buffers, not temporaries. */
    struct url u;
    if(p->opaque_origin||!script_allowed(p,url,policy)||!url_parse(p->origin,&u))return WE_SCHEME;
    if(!strncmp(url,"http:",5)||!strncmp(url,"https:",6)){
        if(strcmp(u.scheme,"http")&&strcmp(u.scheme,"https"))return WE_SCHEME;
        char origin[256];
        if(u.port>=0&&u.port!=url_default_port(u.scheme))snprintf(origin,sizeof origin,"%s://%s:%d",u.scheme,u.host,u.port);
        else snprintf(origin,sizeof origin,"%s://%s",u.scheme,u.host);
        return ld_start_web(url,F_MODULE,origin,NULL,NULL,1,false);
    }
    return ld_start(url,F_MODULE,0,NULL,NULL,false);
}
bool script_module_mime(const char *type)
{
    if(!type)return false;
    size_t n=strcspn(type,"; \t");
    return (n==22&&!strncasecmp(type,"application/javascript",n))||
           (n==15&&!strncasecmp(type,"text/javascript",n))||
           (n==15&&!strncasecmp(type,"text/ecmascript",n));
}
