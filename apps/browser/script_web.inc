/* Native half of js_web.js, included in script.c's guarded VM boundary. */
#define STORAGE_SLOTS 8
#define STORAGE_QUOTA 16384
struct session_record { char *origin, *json; };
static struct session_record sessions[STORAGE_SLOTS];
static char *utf8_arg(jerry_value_t value)
{
    jerry_value_t str=jerry_value_to_string(value);
    if(jerry_value_is_exception(str)){jerry_value_free(str);return NULL;}
    size_t len=jerry_string_size(str,JERRY_ENCODING_UTF8);
    if(len>65536){jerry_value_free(str);return NULL;}
    char *out=temporary(malloc(len+1));
    if(out){jerry_string_to_buffer(str,JERRY_ENCODING_UTF8,(jerry_char_t *)out,(jerry_size_t)len);out[len]=0;
        if(strlen(out)!=len)out=NULL; /* C APIs never accept embedded NULs. */}
    jerry_value_free(str);return out;
}
static jerry_value_t utf8_value(const char *s)
{ return s ? jerry_string((const jerry_char_t *)s,strlen(s),JERRY_ENCODING_UTF8) : jerry_null(); }
static void put_value(jerry_value_t obj,const char *key,jerry_value_t value)
{ jerry_value_free(jerry_object_set_sz(obj,key,value));jerry_value_free(value); }
static void put_string(jerry_value_t obj,const char *key,const char *str)
{ put_value(obj,key,utf8_value(str ? str : "")); }
static char *field_string(jerry_value_t obj,const char *key)
{
    jerry_value_t v=jerry_object_get_sz(obj,key);
    char *out=jerry_value_is_string(v) ? utf8_arg(v) : NULL;
    jerry_value_free(v);return out;
}
static char *web_origin(struct page *p)
{
    struct url *u=temporary(malloc(sizeof *u));
    if(!u || p->opaque_origin || !url_parse(p->origin,u) || !u->host[0] ||
        (strcmp(u->scheme,"http") && strcmp(u->scheme,"https")))return NULL;
    char *out=temporary(malloc(256));if(!out)return NULL;
    if(u->port>=0 && u->port!=url_default_port(u->scheme))
        snprintf(out,256,"%s://%s:%d",u->scheme,u->host,u->port);
    else snprintf(out,256,"%s://%s",u->scheme,u->host);
    return out;
}
static void web_free(struct runtime *s)
{
    for(int i=0;i<WEB_REQUESTS;i++) {
        if(s->requests[i].id)ld_cancel(s->requests[i].id);
        ld_free_result(&s->requests[i].result);
        memset(&s->requests[i],0,sizeof s->requests[i]);
    }
}
/* Persistent records have the exact canonical origin before the JSON. Slot
 * names are numeric; neither hostnames nor page input becomes a file path. */
static int storage(struct page *p,bool local,const char *json,char **read)
{
    char *origin=web_origin(p);if(!origin)return -2;
    if(json && strlen(json)>STORAGE_QUOTA)return -1;
    if(!local){
        int slot=-1,empty=-1;
        for(int i=0;i<STORAGE_SLOTS;i++){
            if(!sessions[i].origin){if(empty<0)empty=i;}
            else if(!strcmp(sessions[i].origin,origin))slot=i;
        }
        if(slot<0){if(!json){*read="[]";return 0;}if(!strcmp(json,"[]"))return 0;slot=empty;}
        if(slot<0)return -1;
        if(json&&!strcmp(json,"[]")){free(sessions[slot].origin);free(sessions[slot].json);memset(&sessions[slot],0,sizeof sessions[slot]);return 0;}
        if(json){
            char *o=strdup(origin),*data=strdup(json);
            if(!o||!data){free(o);free(data);return -1;}
            free(sessions[slot].origin);free(sessions[slot].json);
            sessions[slot].origin=o;sessions[slot].json=data;
        }else *read=sessions[slot].json;
        return 0;
    }
    char *record=temporary(malloc(STORAGE_QUOTA+258));if(!record)return -1;
    int slot=-1,empty=-1;
    for(int i=0;i<STORAGE_SLOTS;i++){
        int len=plat_storage_read(i,record,STORAGE_QUOTA+257);
        if(len<0)return -3;
        if(!len){if(empty<0)empty=i;continue;}
        record[len]=0;
        char *sep=memchr(record,'\n',(size_t)len);
        if(!sep || sep-record>255 || memchr(record,0,(size_t)len))return -3;
        *sep=0;
        if(!strcmp(record,origin)){
            slot=i;if(!json){*read=sep+1;return 0;}break;
        }
    }
    if(!json){*read="[]";return 0;}
    if(slot<0&&!strcmp(json,"[]"))return 0;
    if(slot<0)slot=empty;
    if(slot<0)return -1;
    if(!strcmp(json,"[]"))return plat_storage_write(slot,"",0)<0?-3:0;
    int len=snprintf(record,STORAGE_QUOTA+258,"%s\n%s",origin,json);
    return plat_storage_write(slot,record,len)<0 ? -3 : 0;
}
static void web_layout(struct page *p)
{
    if(p->dirty){page_style(p);page_layout(p,p->width>0?p->width:240);}
    /* Keep dirty set: the UI must repaint this synchronous layout too. */
}
static bool descendant(struct node *child,struct node *parent)
{ for(;child;child=child->parent)if(child==parent)return true;return false; }
static void web_rect(struct page *p,struct node *n,int *r)
{
    memset(r,0,4*sizeof *r);
    if(!node_connected(n))return;
    web_layout(p);bool found=false,box=false;
    for(int i=0;i<p->nitems;i++){
        struct ditem *d=&p->items[i];
        if(d->node==n && d->kind==DI_BOX){r[0]=d->x;r[1]=d->y;r[2]=d->w;r[3]=d->h;box=true;break;}
        if(d->node==n && d->kind==DI_RECT){r[0]=d->x;r[1]=d->y;r[2]=d->w;r[3]=d->h;box=true;break;}
    }
    if(!box)for(int i=0;i<p->nitems;i++){
        struct ditem *d=&p->items[i];
        if(!d->node||!descendant(d->node,n)||d->w<=0||d->h<=0)continue;
        if(!found){r[0]=d->x;r[1]=d->y;r[2]=d->w;r[3]=d->h;found=true;}
        else {int right=MAX(r[0]+r[2],d->x+d->w),bottom=MAX(r[1]+r[3],d->y+d->h);
            r[0]=MIN(r[0],d->x);r[1]=MIN(r[1],d->y);r[2]=right-r[0];r[3]=bottom-r[1];}
    }
    if(box||found)r[1]-=p->host?p->host->scroll:p->scroll_y;
}
static bool web_native(struct runtime *s,const char *op,jerry_value_t a,jerry_value_t b,
                       struct node *n,jerry_value_t *out)
{
    struct page *p=s->page;
    if(!strcmp(op,"url")){
        char *input=utf8_arg(a),*base=utf8_arg(b);
        struct url *u=temporary(malloc(sizeof *u));
        char *resolved=input&&base?temporary(url_resolve(base,input)):NULL;
        if(!resolved||!u||!url_parse(resolved,u)||!u->host[0]||
            (strcmp(u->scheme,"http")&&strcmp(u->scheme,"https"))){*out=jerry_null();return true;}
        if(u->port==url_default_port(u->scheme))u->port=-1;
        if(!u->path[0])strcpy(u->path,"/");
        char *buf=temporary(malloc(4096));if(!buf)return true;
        url_format(u,buf,4096,true);*out=jerry_object();put_string(*out,"href",buf);
        snprintf(buf,4096,"%s:",u->scheme);put_string(*out,"protocol",buf);
        put_string(*out,"hostname",u->host);
        if(u->port>=0)snprintf(buf,4096,"%s:%d",u->host,u->port);else snprintf(buf,4096,"%s",u->host);
        put_string(*out,"host",buf);
        snprintf(buf,4096,"%s://%s",u->scheme,u->host);
        if(u->port>=0)snprintf(buf,4096,"%s://%s:%d",u->scheme,u->host,u->port);
        put_string(*out,"origin",buf);
        snprintf(buf,4096,u->port>=0?"%d":"",u->port);put_string(*out,"port",buf);
        put_string(*out,"pathname",*u->path?u->path:"/");
        snprintf(buf,4096,"%s%s",u->has_query&&*u->query?"?":"",u->query);put_string(*out,"search",buf);
        snprintf(buf,4096,"%s%s",u->has_frag&&*u->frag?"#":"",u->frag);put_string(*out,"hash",buf);
    }else if(!strcmp(op,"fetch")){
        *out=jerry_number(-1);char *origin=web_origin(p),*input=utf8_arg(a);
        char *url=input?temporary(url_resolve(p->base,input)):NULL;
        if(!origin||!url||!jerry_value_is_object(b)||!policy_for(p,url,NULL,NULL,"connect-src"))return true;
        char *post=field_string(b,"post"),*type=field_string(b,"type"),
             *cred=field_string(b,"credentials"),*mode=field_string(b,"mode");
        if(!cred||!mode)return true;
        int credentials=!strcmp(cred,"omit")?0:!strcmp(cred,"same-origin")?1:!strcmp(cred,"include")?2:-1;
        if(credentials<0 || (strcmp(mode,"cors")&&strcmp(mode,"same-origin")))return true;
        for(int i=0;i<WEB_REQUESTS;i++)if(!s->requests[i].id){
            int id=ld_start_web(url,F_API,origin,post,type,credentials,!strcmp(mode,"same-origin"));
            if(id>0){s->requests[i].id=id;s->requests[i].started=plat_ms();}
            *out=jerry_number(id);break;
        }
    }else if(!strcmp(op,"fetchCancel")){
        int id=jerry_value_is_number(a)?(int)jerry_value_as_number(a):0;
        for(int i=0;i<WEB_REQUESTS;i++)if(id&&s->requests[i].id==id){
            ld_cancel(id);ld_free_result(&s->requests[i].result);memset(&s->requests[i],0,sizeof s->requests[i]);break;
        }
    }else if(!strcmp(op,"fetchPoll")){
        *out=jerry_null();
        for(int i=0;i<WEB_REQUESTS;i++){
            struct web_request *q=&s->requests[i];if(!q->id)continue;
            if(!q->done&&(uint32_t)(plat_ms()-q->started)>30000){ld_cancel(q->id);q->done=true;q->result.err=WE_TIMEOUT;}
            if(!q->done)continue;
            struct fetch_result *r=&q->result;
            *out=jerry_object();put_value(*out,"id",jerry_number(q->id));put_value(*out,"error",jerry_boolean(r->err!=0));
            if(!r->err){
                put_value(*out,"status",jerry_number(r->status));put_string(*out,"url",r->url);
                put_string(*out,"type",r->cors?"cors":"basic");
                put_value(*out,"redirected",jerry_boolean(r->redirected));
                jerry_value_t headers=jerry_object();if(r->ctype)put_string(headers,"content-type",r->ctype);
                put_value(*out,"headers",headers);
                jerry_value_t buf=jerry_arraybuffer((jerry_length_t)r->len);
                jerry_arraybuffer_write(buf,0,r->data,(jerry_length_t)r->len);
                jerry_value_t bytes=jerry_typedarray_with_buffer(JERRY_TYPEDARRAY_UINT8,buf);
                jerry_value_free(buf);put_value(*out,"bytes",bytes);
            }
            ld_free_result(r);memset(q,0,sizeof *q);break;
        }
    }else if(!strcmp(op,"storageGet")||!strcmp(op,"storageSet")){
        char *kind=utf8_arg(a),*json=!strcmp(op,"storageSet")?utf8_arg(b):NULL,*data=NULL;
        int status=kind&&(!strcmp(kind,"local")||!strcmp(kind,"session"))?storage(p,!strcmp(kind,"local"),json,&data):-2;
        *out=!strcmp(op,"storageSet")?jerry_number(json?status:-1):status==-3?jerry_number(-3):status?jerry_null():utf8_value(data);
    }else if(n&&!strcmp(op,"fragment"))*out=wrap(dom_create(n->owner,N_FRAGMENT,NULL,NULL));
    else if(n&&!strcmp(op,"clone"))*out=wrap(dom_clone(n->owner,n,jerry_value_to_boolean(a)));
    else if(n&&!strcmp(op,"rect")){
        int r[4];web_rect(n->owner,n,r);*out=jerry_array(4);
        for(int i=0;i<4;i++)jerry_value_free(jerry_object_set_index(*out,i,jerry_number((double)r[i]*4.0/3.0)));
    }else if(n&&!strcmp(op,"computed")){
        web_layout(n->owner);*out=jerry_null();const struct style *st=n->st;if(!st)return true;
        int r[4];web_rect(n->owner,n,r);*out=jerry_object();char buf[64];
        snprintf(buf,sizeof buf,"rgb(%u, %u, %u)",(st->color>>16)&255,(st->color>>8)&255,st->color&255);put_string(*out,"color",buf);
        if(st->bg)snprintf(buf,sizeof buf,"rgb(%u, %u, %u)",(st->bg>>16)&255,(st->bg>>8)&255,st->bg&255);else snprintf(buf,sizeof buf,"rgba(0, 0, 0, 0)");
        put_string(*out,"backgroundColor",buf);
        snprintf(buf,sizeof buf,"%dpx",st->font_css/4);put_string(*out,"fontSize",buf);
        snprintf(buf,sizeof buf,"%dpx",r[2]*4/3);put_string(*out,"width",buf);
        snprintf(buf,sizeof buf,"%dpx",r[3]*4/3);put_string(*out,"height",buf);
        static const char *display[]={"none","inline","block","inline-block","list-item","table","table-row-group","table-row","table-cell","table-caption","table-column"};
        put_string(*out,"display",st->display<sizeof display/sizeof *display?display[st->display]:"block");
        put_string(*out,"visibility",st->hidden?"hidden":"visible");put_string(*out,"fontWeight",st->bold?"bold":"normal");
    }else return false;
    return true;
}
