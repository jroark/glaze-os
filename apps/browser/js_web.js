/* Bounded Web APIs. Embedded inside the DOM closure by mkjsbootstrap.py. */
function namedError(name,message){var e=new Error(message||name);e.name=name;return e;}
function bounded(s,max){s=String(s);if(s.length>max)throw RangeError('Browser API size limit');return s;}
function TextEncoder(){}
TextEncoder.prototype.encoding='utf-8';
TextEncoder.prototype.encode=function(value){
    var s=bounded(value===undefined?'':value,65536), out=new Uint8Array(Math.min(s.length*3,65536)),used=0;
    function byte(v){if(used>=65536)throw RangeError('Encoded text limit');out[used++]=v;}
    for(var i=0;i<s.length;i++){
        var c=s.charCodeAt(i);
        if(c>=0xd800&&c<=0xdbff){var lo=s.charCodeAt(i+1);if(lo>=0xdc00&&lo<=0xdfff){c=0x10000+((c-0xd800)<<10)+lo-0xdc00;i++;}else c=0xfffd;}
        else if(c>=0xdc00&&c<=0xdfff)c=0xfffd;
        if(c<128)byte(c);
        else if(c<2048){byte(192|(c>>6));byte(128|(c&63));}
        else if(c<65536){byte(224|(c>>12));byte(128|((c>>6)&63));byte(128|(c&63));}
        else {byte(240|(c>>18));byte(128|((c>>12)&63));byte(128|((c>>6)&63));byte(128|(c&63));}
    }
    return out.slice(0,used);
};
function TextDecoder(label,options){
    label=label===undefined?'utf-8':String(label).trim().toLowerCase();
    if(['utf-8','utf8','unicode-1-1-utf-8'].indexOf(label)<0)throw RangeError('Only UTF-8 is supported');
    this.encoding='utf-8';this.fatal=!!(options&&options.fatal);this.ignoreBOM=!!(options&&options.ignoreBOM);this._pending=[];this._started=false;
}
TextDecoder.prototype.decode=function(input,options){
    var bytes=input===undefined?new Uint8Array(0):input instanceof ArrayBuffer?new Uint8Array(input):
        ArrayBuffer.isView(input)?new Uint8Array(input.buffer,input.byteOffset,input.byteLength):null;
    if(!bytes)throw TypeError('Expected an ArrayBuffer or view');
    if(bytes.length+this._pending.length>65536)throw RangeError('Decoded text limit');
    var a=bytes,out='',stream=!!(options&&options.stream);
    if(this._pending.length){a=new Uint8Array(this._pending.length+bytes.length);a.set(this._pending);a.set(bytes,this._pending.length);}this._pending=[];
    for(var i=0;i<a.length;){
        var start=i,c=a[i++],need=0,min=0;
        if(c<128){}
        else if(c>=194&&c<=223){need=1;c&=31;min=128;}
        else if(c>=224&&c<=239){need=2;c&=15;min=2048;}
        else if(c>=240&&c<=244){need=3;c&=7;min=65536;}
        else {if(this.fatal)throw TypeError('Invalid UTF-8');c=0xfffd;}
        if(need){
            var valid=true;
            for(var j=0;j<need;j++){
                if(i>=a.length){if(stream){this._pending=Array.prototype.slice.call(a,start);return out;}valid=false;break;}
                var b=a[i];
                if(b<128||b>191||(j===0&&((a[start]===224&&b<160)||(a[start]===237&&b>159)||(a[start]===240&&b<144)||(a[start]===244&&b>143)))){valid=false;break;}
                i++;c=(c<<6)|(b&63);
            }
            if(!valid||c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff)){if(this.fatal)throw TypeError('Invalid UTF-8');c=0xfffd;}
        }
        if(!this._started){this._started=true;if(c===0xfeff&&!this.ignoreBOM)continue;}
        if(c>65535){c-=65536;out+=String.fromCharCode(0xd800+(c>>10),0xdc00+(c&1023));}else out+=String.fromCharCode(c);
    }
    if(!stream){this._pending=[];this._started=false;}
    return out;
};
win.TextEncoder=TextEncoder;win.TextDecoder=TextDecoder;
function usv(s){return new TextDecoder('utf-8',{ignoreBOM:true}).decode(new TextEncoder().encode(s));}
function formEncode(s){return encodeURIComponent(usv(s)).replace(/[!'()~]/g,function(c){return '%'+c.charCodeAt(0).toString(16).toUpperCase();}).replace(/%20/g,'+');}
function formDecode(s){
    s=s.replace(/\+/g,' ');var bytes=[];
    for(var i=0;i<s.length;i++){
        if(s[i]==='%'&&/^[0-9a-f]{2}$/i.test(s.slice(i+1,i+3))){bytes.push(parseInt(s.slice(i+1,i+3),16));i+=2;}
        else {var c=s.charCodeAt(i),chunk=s[i];if(c>=0xd800&&c<=0xdbff&&i+1<s.length)chunk+=s[++i];var a=new TextEncoder().encode(chunk);for(var j=0;j<a.length;j++)bytes.push(a[j]);}
    }
    return new TextDecoder('utf-8',{ignoreBOM:true}).decode(new Uint8Array(bytes));
}
function URLSearchParams(init){
    this._list=[];
    if(init===undefined||init===null)return;
    if(typeof init==='string'){
        bounded(init,16384).replace(/^\?/,'').split('&').forEach(function(pair){if(!pair)return;var i=pair.indexOf('=');this.append(formDecode(i<0?pair:pair.slice(0,i)),formDecode(i<0?'':pair.slice(i+1)));},this);
    }else if(init[Symbol.iterator]){
        for(var entry of init){var a=Array.from(entry);if(a.length!==2)throw TypeError('Expected pairs');this.append(a[0],a[1]);}
    }else Object.keys(init).forEach(function(k){this.append(k,init[k]);},this);
}
URLSearchParams.prototype._change=function(){if(this._changed)this._changed();};
URLSearchParams.prototype.append=function(k,v){if(this._list.length>=128)throw RangeError('Too many query parameters');this._list.push([usv(bounded(k,4096)),usv(bounded(v,4096))]);this._change();};
URLSearchParams.prototype.get=function(k){k=String(k);for(var i=0;i<this._list.length;i++)if(this._list[i][0]===k)return this._list[i][1];return null;};
URLSearchParams.prototype.getAll=function(k){k=String(k);return this._list.filter(function(p){return p[0]===k;}).map(function(p){return p[1];});};
URLSearchParams.prototype.has=function(k,v){k=String(k);return this._list.some(function(p){return p[0]===k&&(v===undefined||p[1]===String(v));});};
URLSearchParams.prototype.delete=function(k,v){k=String(k);this._list=this._list.filter(function(p){return p[0]!==k||(v!==undefined&&p[1]!==String(v));});this._change();};
URLSearchParams.prototype.set=function(k,v){k=usv(bounded(k,4096));v=usv(bounded(v,4096));var found=false;this._list=this._list.filter(function(p){if(p[0]!==k)return true;if(found)return false;found=true;p[1]=v;return true;});if(!found)this.append(k,v);else this._change();};
URLSearchParams.prototype.sort=function(){this._list.sort(function(a,b){return a[0]<b[0]?-1:a[0]>b[0]?1:0;});this._change();};
URLSearchParams.prototype.toString=function(){return this._list.map(function(p){return formEncode(p[0])+'='+formEncode(p[1]);}).join('&');};
URLSearchParams.prototype.forEach=function(fn,ctx){this._list.forEach(function(p){fn.call(ctx,p[1],p[0],this);},this);};
URLSearchParams.prototype.entries=function(){return this._list.map(function(p){return p.slice();})[Symbol.iterator]();};
URLSearchParams.prototype.keys=function(){return this._list.map(function(p){return p[0];})[Symbol.iterator]();};
URLSearchParams.prototype.values=function(){return this._list.map(function(p){return p[1];})[Symbol.iterator]();};
URLSearchParams.prototype[Symbol.iterator]=URLSearchParams.prototype.entries;
Object.defineProperty(URLSearchParams.prototype,'size',{get:function(){return this._list.length;}});
win.URLSearchParams=URLSearchParams;
function urlInput(s){return bounded(s,4096).replace(/[^\x00-\x7f]+/g,function(x){return encodeURI(usv(x));});}
function URL(input,base){
    this._parts=null;this._params=new URLSearchParams();var self=this;
    this._set(input,base===undefined?'':base);
    this._params._changed=function(){self.search=self._params.toString();};
}
URL.prototype._set=function(input,base){
    var p=dom('url',null,urlInput(input),urlInput(base));if(!p)throw TypeError('Invalid or unsupported URL');
    this._parts=p;this._params._list=new URLSearchParams(p.search)._list;
};
Object.defineProperty(URL.prototype,'href',{get:function(){return this._parts.href;},set:function(v){this._set(v,'');}});
Object.defineProperty(URL.prototype,'origin',{get:function(){return this._parts.origin;}});
Object.defineProperty(URL.prototype,'searchParams',{get:function(){return this._params;}});
['protocol','host','hostname','port','pathname','search','hash'].forEach(function(k){Object.defineProperty(URL.prototype,k,{get:function(){return this._parts[k];},set:function(v){
    var p=Object.assign({},this._parts);p[k]=String(v);
    if(k==='hostname')p.host=p.hostname+(p.port?':'+p.port:'');
    if(k==='port')p.host=p.hostname+(p.port?':'+p.port:'');
    var protocol=p.protocol.replace(/:$/,'')+':';
    var path=p.pathname||'/';if(path[0]!=='/')path='/'+path;
    this._set(protocol+'//'+p.host+path+(p.search?'?'+p.search.replace(/^\?/,''):'')+(p.hash?'#'+p.hash.replace(/^#/,''):''),'');
}});});
URL.prototype.toString=URL.prototype.toJSON=function(){return this.href;};
URL.canParse=function(u,b){try{new URL(u,b);return true;}catch(e){return false;}};
win.URL=URL;
function EventTarget(){}
EventTarget.prototype.addEventListener=add;EventTarget.prototype.removeEventListener=remove;
EventTarget.prototype.dispatchEvent=function(e){return dispatch(this,e.type,e);};
win.EventTarget=EventTarget;
function AbortSignal(){this._aborted=false;this._reason=undefined;}
AbortSignal.prototype=Object.create(EventTarget.prototype);
Object.defineProperty(AbortSignal.prototype,'aborted',{get:function(){return this._aborted;}});
Object.defineProperty(AbortSignal.prototype,'reason',{get:function(){return this._reason;}});
AbortSignal.prototype.throwIfAborted=function(){if(this.aborted)throw this.reason;};
function AbortController(){this.signal=new AbortSignal();}
AbortController.prototype.abort=function(reason){var s=this.signal;if(s.aborted)return;s._aborted=true;s._reason=reason===undefined?namedError('AbortError','Request aborted'):reason;s.dispatchEvent(new Event('abort'));};
AbortSignal.abort=function(reason){var c=new AbortController();c.abort(reason);return c.signal;};
win.AbortController=AbortController;win.AbortSignal=AbortSignal;
function Headers(init){this._data=Object.create(null);if(init instanceof Headers)init.forEach(function(v,k){this.append(k,v);},this);else if(init&&init[Symbol.iterator]){for(var pair of init){if(pair.length!==2)throw TypeError('Header pairs');this.append(pair[0],pair[1]);}}else if(init)Object.keys(init).forEach(function(k){this.append(k,init[k]);},this);}
function headerName(n){n=String(n).toLowerCase();if(!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(n))throw TypeError('Invalid header name');return n;}
Headers.prototype.append=function(k,v){k=headerName(k);v=bounded(v,512).trim();if(/[\x00-\x08\x0a-\x1f\x7f]/.test(v))throw TypeError('Invalid header value');this._data[k]=this.has(k)?this._data[k]+', '+v:v;};
Headers.prototype.set=function(k,v){this.delete(k);this.append(k,v);};
Headers.prototype.get=function(k){k=headerName(k);return Object.prototype.hasOwnProperty.call(this._data,k)?this._data[k]:null;};
Headers.prototype.has=function(k){return this.get(k)!==null;};
Headers.prototype.delete=function(k){delete this._data[headerName(k)];};
Headers.prototype.forEach=function(fn,ctx){Object.keys(this._data).sort().forEach(function(k){fn.call(ctx,this._data[k],k,this);},this);};
Headers.prototype.entries=function(){var self=this;return Object.keys(this._data).sort().map(function(k){return [k,self._data[k]];})[Symbol.iterator]();};
Headers.prototype[Symbol.iterator]=Headers.prototype.entries;win.Headers=Headers;
function Response(bytes,info){this._bytes=bytes;this.status=info.status;this.ok=this.status>=200&&this.status<300;this.url=info.url;this.redirected=!!info.redirected;this.type=info.type;this.headers=new Headers(info.headers);this.bodyUsed=false;}
Response.prototype._consume=function(){if(this.bodyUsed)throw TypeError('Body already consumed');this.bodyUsed=true;var a=this._bytes;this._bytes=null;return a;};
Response.prototype.text=function(){try{return Promise.resolve(new TextDecoder().decode(this._consume()));}catch(e){return Promise.reject(e);}};
Response.prototype.json=function(){return this.text().then(JSON.parse);};
Response.prototype.arrayBuffer=function(){try{return Promise.resolve(this._consume().buffer);}catch(e){return Promise.reject(e);}};
Response.prototype.clone=function(){if(this.bodyUsed)throw TypeError('Body already consumed');return new Response(this._bytes.slice(),this);};
var requests=Object.create(null);
win.fetch=function(input,options){return new Promise(function(resolve,reject){
    var o=options||{},method=String(o.method||'GET').toUpperCase(),id=0,signal=o.signal,abort;
    try{
        if(signal)signal.throwIfAborted();
        if(method!=='GET'&&method!=='POST')throw TypeError('Only GET and POST are supported');
        var headers=new Headers(o.headers),type=headers.get('content-type');
        headers.forEach(function(v,k){if(k!=='content-type')throw TypeError('Only Content-Type request headers are supported');});
        if(o.body!==undefined&&o.body!==null&&typeof o.body!=='string'&&!(o.body instanceof URLSearchParams))throw TypeError('Only text and URLSearchParams bodies are supported');
        var body=o.body===undefined||o.body===null?null:o.body instanceof URLSearchParams?o.body.toString():bounded(o.body,16384);
        if(body!==null&&(body.indexOf('\0')>=0||new TextEncoder().encode(body).length>16384))throw TypeError('Request body exceeds text limits');
        if(method==='GET'&&body!==null)throw TypeError('GET cannot have a body');
        if(!type&&body!==null)type=o.body instanceof URLSearchParams?'application/x-www-form-urlencoded':'text/plain;charset=UTF-8';
        if(type&&!/^[\x20-\x7e]{1,128}$/.test(type))throw TypeError('Invalid content type');
        var credentials=o.credentials===undefined?'same-origin':String(o.credentials),mode=o.mode===undefined?'cors':String(o.mode);
        if(['omit','same-origin','include'].indexOf(credentials)<0||['cors','same-origin'].indexOf(mode)<0)throw TypeError('Unsupported fetch mode');
        if(o.redirect&&o.redirect!=='follow')throw TypeError('Only follow redirects are supported');
        id=dom('fetch',null,urlInput(input),{post:method==='POST'?(body===null?'':body):null,type:type,credentials:credentials,mode:mode});
        if(!(id>0))throw TypeError('Request blocked or request limit reached');
        abort=function(){if(requests[id]){delete requests[id];dom('fetchCancel',null,id);if(signal)signal.removeEventListener('abort',abort);reject(signal.reason);}};
        requests[id]={resolve:resolve,reject:reject,signal:signal,abort:abort};
        if(signal)signal.addEventListener('abort',abort,{once:true});
    }catch(e){if(id>0)dom('fetchCancel',null,id);reject(e);}
});};
var storageKinds=new WeakMap();
function Storage(){throw TypeError('Illegal constructor');}
Storage.prototype._read=function(){var kind=storageKinds.get(this);if(!kind)throw TypeError('Illegal invocation');var s=dom('storageGet',null,kind);if(typeof s==='number')throw namedError('UnknownError','Storage read failed');if(s===null)throw namedError('SecurityError','Storage requires an HTTP(S) origin');return JSON.parse(s||'[]');};
Storage.prototype._write=function(a){if(a.length>64)throw namedError('QuotaExceededError');var status=dom('storageSet',null,storageKinds.get(this),JSON.stringify(a));if(status!==0)throw namedError(status===-2?'SecurityError':status===-3?'UnknownError':'QuotaExceededError');};
Object.defineProperty(Storage.prototype,'length',{get:function(){return this._read().length;}});
Storage.prototype.key=function(i){var a=this._read();i=Number(i)>>>0;return i<a.length?a[i][0]:null;};
Storage.prototype.getItem=function(k){k=String(k);var a=this._read();for(var i=0;i<a.length;i++)if(a[i][0]===k)return a[i][1];return null;};
Storage.prototype.setItem=function(k,v){k=String(k);v=String(v);if(k.length>16384||v.length>16384)throw namedError('QuotaExceededError');var a=this._read(),found=false;for(var i=0;i<a.length;i++)if(a[i][0]===k){a[i][1]=v;found=true;break;}if(!found)a.push([k,v]);this._write(a);};
Storage.prototype.removeItem=function(k){k=String(k);this._write(this._read().filter(function(p){return p[0]!==k;}));};
Storage.prototype.clear=function(){this._write([]);};
function storage(kind){
    var target=Object.create(Storage.prototype),proxy=new Proxy(target,{
        get:function(t,k){if(k in t||typeof k==='symbol')return t[k];var v=proxy.getItem(k);return v===null?undefined:v;},
        set:function(t,k,v){proxy.setItem(k,v);return true;},
        deleteProperty:function(t,k){proxy.removeItem(k);return true;},
        ownKeys:function(){return proxy._read().map(function(p){return p[0];});},
        getOwnPropertyDescriptor:function(t,k){var v=proxy.getItem(k);return v===null?undefined:{value:v,writable:true,enumerable:true,configurable:true};}
    });
    storageKinds.set(target,kind);storageKinds.set(proxy,kind);return proxy;
}
var local=storage('local'),session=storage('session');
['localStorage','sessionStorage'].forEach(function(k){Object.defineProperty(win,k,{get:function(){var s=k==='localStorage'?local:session;s._read();return s;}});});
win.Storage=Storage;
proto.getBoundingClientRect=function(){var a=dom('rect',this)||[0,0,0,0];return {x:a[0],y:a[1],left:a[0],top:a[1],width:a[2],height:a[3],right:a[0]+a[2],bottom:a[1]+a[3],toJSON:function(){return {x:this.x,y:this.y,width:this.width,height:this.height,top:this.top,right:this.right,bottom:this.bottom,left:this.left};}};};
win.getComputedStyle=function(el){var data=dom('computed',el);if(!data)throw TypeError('Expected an element');data.getPropertyValue=function(k){return data[k.replace(/-([a-z])/g,function(_,c){return c.toUpperCase();})]||'';};return Object.freeze(data);};
var raf=[],nextRaf=1;
win.requestAnimationFrame=function(fn){if(typeof fn!=='function')throw TypeError('Expected a function');if(raf.length>=32)throw RangeError('Animation callback limit');var id=nextRaf++;raf.push({id:id,fn:fn});return id;};
win.cancelAnimationFrame=function(id){raf=raf.filter(function(r){return r.id!==id;});};
function webTick(){
    for(var i=0;i<4;i++){
        var r=dom('fetchPoll');if(!r)break;
        var pending=requests[r.id];if(!pending)continue;delete requests[r.id];
        if(pending.signal)pending.signal.removeEventListener('abort',pending.abort);
        if(r.error)pending.reject(TypeError('Network request failed or was blocked'));
        else pending.resolve(new Response(r.bytes,r));
    }
    var copy=raf.slice();var now=dom('performanceNow');
    for(var i=0;i<copy.length;i++){var index=raf.indexOf(copy[i]);if(index<0)continue;raf.splice(index,1);try{copy[i].fn(now);}catch(e){console.error(e);}}
}
