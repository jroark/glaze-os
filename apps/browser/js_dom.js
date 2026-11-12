/* Small DOM facade. Native node identities survive edits, but expire on unload. */
(function (native) {
    'use strict';
    var win = globalThis, listeners = [], timers = [], nextTimer = 1;
    function dom(op, node, a, b) { return native(op, node, a, b); }
    function Node() {}
    var proto = Node.prototype;
    function prop(name, get, set) { Object.defineProperty(proto, name, {get:get, set:set, configurable:true}); }
    function attr(name, key) {
        prop(name, function () { return this.getAttribute(key || name) || ''; },
             function (v) { this.setAttribute(key || name, v); });
    }
    prop('nodeType', function () { return dom('type', this); });
    prop('nodeName', function () { return dom('name', this); });
    prop('tagName', function () { return this.nodeType === 1 ? this.nodeName : undefined; });
    prop('ownerDocument', function () { return this.nodeType === 9 ? null : dom('document', this); });
    prop('parentNode', function () { return dom('parent', this); });
    prop('parentElement', function () { var p=this.parentNode; return p && p.nodeType===1 ? p : null; });
    prop('firstChild', function () { return dom('first', this); });
    prop('lastChild', function () { return dom('last', this); });
    prop('nextSibling', function () { return dom('next', this); });
    prop('previousSibling', function () { return dom('prev', this); });
    prop('childNodes', function () { return dom('children', this); });
    prop('children', function () { return this.childNodes.filter(function(n){return n.nodeType===1;}); });
    prop('firstElementChild', function () { return this.children[0] || null; });
    prop('lastElementChild', function () { var a=this.children; return a[a.length-1] || null; });
    prop('textContent', function () { return dom('text', this); }, function(v){dom('text=',this,String(v));});
    prop('innerText', function () { return this.textContent; }, function(v){this.textContent=v;});
    prop('nodeValue', function () { return this.nodeType===3 ? this.textContent : null; },
         function(v){if(this.nodeType===3)this.textContent=v;});
    prop('innerHTML', function () { return dom('html',this); },function(v){dom('html=',this,String(v));});
    prop('outerHTML', function () { return dom('outer',this); });
    prop('isConnected', function () { return dom('connected',this); });
    proto.getAttribute=function(n){return dom('attr',this,String(n));};
    proto.hasAttribute=function(n){return this.getAttribute(n)!==null;};
    proto.setAttribute=function(n,v){dom('attr=',this,String(n),String(v));};
    proto.removeAttribute=function(n){dom('attr-',this,String(n));};
    proto.appendChild=function(n){return dom('insert',this,n,null);};
    proto.insertBefore=function(n,b){return dom('insert',this,n,b);};
    proto.removeChild=function(n){if(n.parentNode!==this)throw Error('Not a child');dom('remove',n);return n;};
    proto.remove=function(){dom('remove',this);};
    proto.cloneNode=function(deep){return dom('clone',this,!!deep);};
    proto.replaceChild=function(n,o){this.insertBefore(n,o);this.removeChild(o);return o;};
    proto.querySelectorAll=function(s){return dom('query',this,String(s));};
    proto.querySelector=function(s){return this.querySelectorAll(s)[0]||null;};
    proto.getElementsByTagName=function(s){return this.querySelectorAll(String(s));};
    proto.getElementsByClassName=function(s){return this.querySelectorAll('.'+String(s).trim().split(/\s+/).join('.'));};
    proto.contains=function(n){while(n){if(n===this)return true;n=n.parentNode;}return false;};
    ['id','name','href','src','srcdoc','target','type','action','method','title','width','height'].forEach(function(n){attr(n);});
    attr('className','class');
    ['value','checked','disabled','selectedIndex'].forEach(function(n){
        prop(n,function(){return dom(n,this);},function(v){dom(n+'=',this,v);});
    });
    prop('classList',function(){
        var e=this;
        function list(){return e.className.trim().split(/\s+/).filter(Boolean);}
        function put(a){e.className=a.join(' ');}
        var l={contains:function(c){return list().indexOf(String(c))>=0;},
            add:function(){var a=list();for(var i=0;i<arguments.length;i++)if(a.indexOf(String(arguments[i]))<0)a.push(String(arguments[i]));put(a);},
            remove:function(){var r=Array.prototype.map.call(arguments,String);put(list().filter(function(x){return r.indexOf(x)<0;}));},
            toggle:function(c,force){var has=this.contains(c), add=force===undefined?!has:!!force;
                if(add)this.add(c);else this.remove(c);return add;},
            replace:function(o,n){var a=list(),i=a.indexOf(String(o));if(i<0)return false;a[i]=String(n);put(a);return true;},
            item:function(i){return list()[i]||null;},forEach:function(f,t){list().forEach(f,t);},
            toString:function(){return e.className;}};
        Object.defineProperty(l,'length',{get:function(){return list().length;}});
        Object.defineProperty(l,'value',{get:function(){return e.className;},set:function(v){e.className=v;}});
        return l;
    });
    prop('style',function(){
        if(this._style)return this._style;
        var e=this, s={};
        function parse(){var o={};(e.getAttribute('style')||'').split(';').forEach(function(v){var i=v.indexOf(':');if(i>0)o[v.slice(0,i).trim()]=v.slice(i+1).trim();});return o;}
        s.getPropertyValue=function(n){return parse()[n]||'';};
        s.setProperty=function(n,v){var o=parse();o[n]=String(v);e.setAttribute('style',Object.keys(o).map(function(k){return k+':'+o[k];}).join(';'));};
        s.removeProperty=function(n){var o=parse(),v=o[n];delete o[n];e.setAttribute('style',Object.keys(o).map(function(k){return k+':'+o[k];}).join(';'));return v||'';};
        Object.defineProperty(s,'cssText',{get:function(){return e.getAttribute('style')||'';},set:function(v){e.setAttribute('style',v);}});
        ['display','color','background','backgroundColor','backgroundImage','fontSize','fontWeight','fontStyle','fontFamily','lineHeight','width','height','maxWidth','minWidth','maxHeight','margin','marginTop','marginRight','marginBottom','marginLeft','padding','paddingTop','paddingRight','paddingBottom','paddingLeft','border','textAlign','textDecoration','verticalAlign','whiteSpace','visibility','position','left','top','right','bottom','opacity','overflow','transform','float','cssFloat','zIndex','cursor'].forEach(function(k){var css=k.replace(/[A-Z]/g,function(c){return '-'+c.toLowerCase();});Object.defineProperty(s,k,{get:function(){return s.getPropertyValue(css);},set:function(v){s.setProperty(css,v);}});});
        this._style=s;return s;
    });
    prop('contentDocument',function(){return dom('frameDocument',this);});
    prop('contentWindow',function(){return dom('frameWindow',this);});
    /* Everyday element methods; a page calls these without testing for them. */
    proto.matches=proto.webkitMatchesSelector=proto.msMatchesSelector=function(s){return dom('matches',this,String(s));};
    proto.closest=function(s){for(var n=this;n&&n.nodeType===1;n=n.parentNode)if(n.matches(s))return n;return null;};
    proto.hasChildNodes=function(){return !!this.firstChild;};
    proto.hasAttributes=function(){return dom('attrNames',this).length>0;};
    proto.getAttributeNames=function(){return dom('attrNames',this);};
    proto.toggleAttribute=function(n,force){var has=this.hasAttribute(n),on=force===undefined?!has:!!force;
        if(on)this.setAttribute(n,'');else this.removeAttribute(n);return on;};
    proto.getAttributeNode=function(n){var v=this.getAttribute(n);return v===null?null:{name:String(n),value:v,specified:true};};
    prop('attributes',function(){var e=this,a=dom('attrNames',e).map(function(n){return {name:n,value:e.getAttribute(n),specified:true};});
        a.getNamedItem=function(n){return e.getAttributeNode(n);};a.item=function(i){return a[i]||null;};return a;});
    prop('childElementCount',function(){return this.children.length;});
    function sibling(n,key){do n=n[key];while(n&&n.nodeType!==1);return n||null;}
    prop('nextElementSibling',function(){return sibling(this,'nextSibling');});
    prop('previousElementSibling',function(){return sibling(this,'previousSibling');});
    prop('localName',function(){return this.nodeType===1?this.nodeName.toLowerCase():undefined;});
    prop('hidden',function(){return this.hasAttribute('hidden');},function(v){this.toggleAttribute('hidden',!!v);});
    prop('dataset',function(){var e=this;
        function key(k){return 'data-'+String(k).replace(/[A-Z]/g,function(c){return '-'+c.toLowerCase();});}
        return new Proxy({},{get:function(t,k){if(typeof k!=='string')return undefined;var v=e.getAttribute(key(k));return v===null?undefined:v;},
            set:function(t,k,v){e.setAttribute(key(k),String(v));return true;},
            has:function(t,k){return typeof k==='string'&&e.hasAttribute(key(k));},
            deleteProperty:function(t,k){e.removeAttribute(key(k));return true;},
            ownKeys:function(){return dom('attrNames',e).filter(function(n){return n.indexOf('data-')===0;}).map(function(n){return n.slice(5).replace(/-([a-z])/g,function(m,c){return c.toUpperCase();});});},
            getOwnPropertyDescriptor:function(t,k){var v=e.getAttribute(key(k));return v===null?undefined:{value:v,writable:true,enumerable:true,configurable:true};}});});
    function nodes(args){var f=document.createDocumentFragment();
        for(var i=0;i<args.length;i++)f.appendChild(args[i]&&typeof args[i]==='object'?args[i]:document.createTextNode(String(args[i])));return f;}
    proto.append=function(){this.appendChild(nodes(arguments));};
    proto.prepend=function(){this.insertBefore(nodes(arguments),this.firstChild);};
    proto.before=function(){if(this.parentNode)this.parentNode.insertBefore(nodes(arguments),this);};
    proto.after=function(){if(this.parentNode)this.parentNode.insertBefore(nodes(arguments),this.nextSibling);};
    proto.replaceWith=function(){var p=this.parentNode;if(p){p.insertBefore(nodes(arguments),this);p.removeChild(this);}};
    proto.replaceChildren=function(){while(this.firstChild)this.removeChild(this.firstChild);this.appendChild(nodes(arguments));};
    proto.insertAdjacentElement=function(where,n){where=String(where).toLowerCase();
        if(where==='beforebegin'){if(this.parentNode)this.parentNode.insertBefore(n,this);}
        else if(where==='afterbegin')this.insertBefore(n,this.firstChild);
        else if(where==='beforeend')this.appendChild(n);
        else if(where==='afterend'){if(this.parentNode)this.parentNode.insertBefore(n,this.nextSibling);}
        else throw SyntaxError('Unknown position');return n;};
    proto.insertAdjacentHTML=function(where,html){var t=document.createElement('div'),f=document.createDocumentFragment();
        t.innerHTML=String(html);while(t.firstChild)f.appendChild(t.firstChild);this.insertAdjacentElement(where,f);};
    proto.insertAdjacentText=function(where,text){this.insertAdjacentElement(where,document.createTextNode(String(text)));};
    proto.isSameNode=function(n){return n===this;};
    proto.getRootNode=function(){for(var n=this;n.parentNode;)n=n.parentNode;return n;};
    proto.normalize=proto.focus=proto.blur=proto.select=proto.setSelectionRange=function(){};
    proto.compareDocumentPosition=function(o){if(o===this)return 0;if(this.contains(o))return 20;if(o.contains&&o.contains(this))return 10;
        var all=document.querySelectorAll('*'),a=all.indexOf(this),b=all.indexOf(o);return a<0||b<0?1:a<b?4:2;};
    ['ELEMENT_NODE',1,'TEXT_NODE',3,'COMMENT_NODE',8,'DOCUMENT_NODE',9,'DOCUMENT_FRAGMENT_NODE',11].forEach(function(v,i,a){if(i%2===0)Node[v]=proto[v]=a[i+1];});
    proto.submit=function(){dom('submit',this);};
    proto.reset=function(){dom('reset',this);};
    function add(type,fn,options){
        if(!(typeof fn==='function'||fn&&typeof fn.handleEvent==='function'))return;
        var capture=typeof options==='boolean'?options:!!(options&&options.capture);
        type=String(type);
        for(var i=0;i<listeners.length;i++)if(listeners[i].target===this&&listeners[i].type===type&&listeners[i].fn===fn&&listeners[i].capture===capture)return;
        if(options&&options.signal&&options.signal.aborted)return;
        var rec={target:this,type:type,fn:fn,capture:capture,once:!!(options&&options.once),passive:!!(options&&options.passive)};
        listeners.push(rec);
        if(options&&options.signal){rec.signal=options.signal;rec.abort=function(){dropListener(rec);};rec.signal.addEventListener('abort',rec.abort,{once:true});}
    }
    function dropListener(rec){var i=listeners.indexOf(rec);if(i>=0)listeners.splice(i,1);if(rec.signal)rec.signal.removeEventListener('abort',rec.abort);}
    function remove(type,fn,options){var capture=typeof options==='boolean'?options:!!(options&&options.capture);for(var i=listeners.length-1;i>=0;i--)if(listeners[i].target===this&&listeners[i].type===String(type)&&listeners[i].fn===fn&&listeners[i].capture===capture)dropListener(listeners[i]);}
    function Event(type,options){this.type=String(type);this.bubbles=!!(options&&options.bubbles);this.cancelable=!!(options&&options.cancelable);this.defaultPrevented=false;this._stop=false;this._immediate=false;this.eventPhase=0;this.timeStamp=dom('performanceNow');}
    Event.prototype.preventDefault=function(){if(this.cancelable&&!this._passive)this.defaultPrevented=true;};
    Event.prototype.stopPropagation=function(){this._stop=true;};
    Event.prototype.stopImmediatePropagation=function(){this._stop=this._immediate=true;};
    Event.prototype.composedPath=function(){return (this._path||[]).slice();};
    function dispatch(target,type,event){
        var e=event||new Event(type,{bubbles:type!=='load'&&type!=='focus'&&type!=='blur',cancelable:true});
        if(e.eventPhase)throw Error('Event is already being dispatched');
        e.target=target;e._stop=e._immediate=false;
        var path=[target];for(var n=target;n&&n!==win;){n=n===document?win:n.parentNode;if(n)path.push(n);}
        e._path=path.slice();
        function invoke(n,capture,phase){
            e.currentTarget=n;e.eventPhase=phase;
            var copy=listeners.slice();
            for(var i=0;i<copy.length&&!e._immediate;i++){
                var rec=copy[i];
                if(rec.target!==n||rec.type!==e.type||rec.capture!==capture||listeners.indexOf(rec)<0)continue;
                if(rec.once)dropListener(rec);
                e._passive=rec.passive;
                try{if(typeof rec.fn==='function')rec.fn.call(n,e);else rec.fn.handleEvent(e);}catch(err){console.error(err);}
                e._passive=false;
            }
            if(!capture&&!e._immediate){
                var fn=n['on'+e.type];
                if(!fn&&n.nodeType===1){var source=n.getAttribute('on'+e.type);if(source)fn=dom('handler',n,source);}
                try{if(typeof fn==='function'&&fn.call(n,e)===false)e.preventDefault();}catch(err){console.error(err);}
            }
        }
        try{
            for(var i=path.length-1;i>0&&!e._stop;i--)invoke(path[i],true,1);
            if(!e._stop){invoke(target,true,2);if(!e._immediate)invoke(target,false,2);}
            if(e.bubbles)for(var i=1;i<path.length&&!e._stop;i++)invoke(path[i],false,3);
        }finally{e.currentTarget=null;e.eventPhase=0;e._passive=false;}
        return !e.defaultPrevented;
    }
    Event.prototype.initEvent=function(type,bubbles,cancelable){this.type=String(type);this.bubbles=!!bubbles;this.cancelable=!!cancelable;};
    Event.prototype.NONE=0;Event.prototype.CAPTURING_PHASE=1;Event.prototype.AT_TARGET=2;Event.prototype.BUBBLING_PHASE=3;
    Object.defineProperty(Event.prototype,'srcElement',{get:function(){return this.target;},configurable:true});
    Object.defineProperty(Event.prototype,'returnValue',{get:function(){return !this.defaultPrevented;},set:function(v){if(!v)this.preventDefault();},configurable:true});
    Object.defineProperty(Event.prototype,'cancelBubble',{get:function(){return this._stop;},set:function(v){if(v)this._stop=true;},configurable:true});
    function eventKind(name,fields){
        var K=function(type,options){Event.call(this,type,options);var o=options||{};
            for(var k in fields)this[k]=k in o?o[k]:fields[k];};
        K.prototype=Object.create(Event.prototype);K.prototype.constructor=K;win[name]=K;return K;}
    var CustomEvent=eventKind('CustomEvent',{detail:null});
    CustomEvent.prototype.initCustomEvent=function(type,bubbles,cancelable,detail){this.initEvent(type,bubbles,cancelable);this.detail=detail;};
    eventKind('UIEvent',{detail:0,view:null});
    eventKind('MouseEvent',{clientX:0,clientY:0,pageX:0,pageY:0,screenX:0,screenY:0,button:0,buttons:0,ctrlKey:false,shiftKey:false,altKey:false,metaKey:false,relatedTarget:null});
    eventKind('KeyboardEvent',{key:'',code:'',keyCode:0,which:0,ctrlKey:false,shiftKey:false,altKey:false,metaKey:false,repeat:false});
    eventKind('FocusEvent',{relatedTarget:null});
    proto.addEventListener=win.addEventListener=add;
    proto.removeEventListener=win.removeEventListener=remove;
    proto.dispatchEvent=win.dispatchEvent=function(e){return dispatch(this,e.type,e);};
    proto.click=function(){dispatch(this,'click');};
    var document=dom('document',null);Object.setPrototypeOf(document,proto);
    document.createElement=function(n){return dom('create',document,String(n));};
    document.createTextNode=function(s){return dom('createText',document,String(s));};
    document.createDocumentFragment=function(){return dom('fragment',document);};
    document.getElementById=function(id){return dom('id',document,String(id));};
    document.getElementsByName=function(n){return document.querySelectorAll('[name="'+String(n).replace(/["\\]/g,'\\$&')+'"]');};
    document.createComment=function(){return document.createTextNode('');};
    document.createEvent=function(kind){var K=win[String(kind).replace(/s$/,'')];var e=new (typeof K==='function'&&K.prototype instanceof Event?K:Event)('');e.type='';return e;};
    [['forms','form'],['images','img'],['links','a[href],area[href]'],['scripts','script'],['anchors','a[name]'],['embeds','embed']].forEach(function(c){
        Object.defineProperty(document,c[0],{get:function(){return document.querySelectorAll(c[1]);},configurable:true});});
    prop('elements',function(){var f=this,a=document.querySelectorAll('input,select,textarea,button').filter(function(c){
            var id=c.getAttribute('form');return id?id===f.id:f.contains(c);});
        a.forEach(function(c){var n=c.name||c.id;if(n&&!(n in a))a[n]=c;});a.namedItem=function(n){return a[n]||null;};return a;});
    prop('form',function(){var id=this.getAttribute('form');return id?document.getElementById(id):this.closest('form');});
    prop('options',function(){return this.querySelectorAll('option');});
    prop('labels',function(){var e=this;return document.querySelectorAll('label').filter(function(l){return l.htmlFor?l.htmlFor===e.id:l.contains(e);});});
    attr('htmlFor','for');attr('placeholder');attr('alt');attr('rel');attr('lang');attr('dir');attr('role');attr('tabIndex','tabindex');
    Object.defineProperty(document,'activeElement',{get:function(){return document.body;},configurable:true});
    Object.defineProperty(document,'hidden',{value:false,configurable:true});
    Object.defineProperty(document,'visibilityState',{value:'visible',configurable:true});
    Object.defineProperty(document,'compatMode',{value:'CSS1Compat',configurable:true});
    Object.defineProperty(document,'characterSet',{value:'UTF-8',configurable:true});
    Object.defineProperty(document,'referrer',{value:'',configurable:true});
    Object.defineProperty(document,'domain',{get:function(){return dom('locationPart',null,'hostname');},configurable:true});
    document.hasFocus=function(){return true;};
    /* A detached tree to parse markup into (jQuery tests this as it loads). */
    document.implementation={hasFeature:function(){return true;},createHTMLDocument:function(title){
        var body=document.createElement('body');
        return {body:body,documentElement:body,title:String(title||''),nodeType:9,
            createElement:function(n){return document.createElement(n);},createTextNode:function(t){return document.createTextNode(t);},
            createDocumentFragment:function(){return document.createDocumentFragment();},
            getElementById:function(id){return body.querySelector('#'+id);},querySelector:function(q){return body.querySelector(q);},
            querySelectorAll:function(q){return body.querySelectorAll(q);},getElementsByTagName:function(q){return body.querySelectorAll(q);}};}};
    ['body','head','documentElement','currentScript','readyState','URL'].forEach(function(n){Object.defineProperty(document,n,{get:function(){return dom(n,document);}});});
    Object.defineProperty(document,'title',{get:function(){return dom('title',document);},set:function(v){dom('title=',document,String(v));}});
    Object.defineProperty(document,'cookie',{get:function(){return dom('cookie',document);},set:function(v){dom('cookie=',document,String(v));}});
    document.write=function(){dom('write',document,Array.prototype.join.call(arguments,''));};
    document.writeln=function(){dom('write',document,Array.prototype.join.call(arguments,'')+'\n');};
    var location={assign:function(u){dom('navigate',null,String(u));},replace:function(u){this.assign(u);},reload:function(){this.assign(this.href);},toString:function(){return this.href;}};
    ['protocol','host','hostname','port','pathname','search','hash','origin'].forEach(function(key){Object.defineProperty(location,key,{get:function(){return dom('locationPart',null,key);},
        set:function(v){if(key==='origin')return;var u=new URL(location.href);u[key]=String(v);
            /* A new fragment on the same page is not a load. */
            if(key==='hash'&&u.href.split('#')[0]===location.href.split('#')[0]){var t=document.getElementById(u.hash.slice(1));if(t&&t.scrollIntoView)t.scrollIntoView();return;}
            location.assign(u.href);}});});
    Object.defineProperty(location,'href',{get:function(){return dom('URL',document);},set:function(u){location.assign(u);}});
    Object.defineProperty(win,'location',{get:function(){return location;},set:function(u){location.assign(u);}});
    Object.defineProperty(document,'location',{get:function(){return location;},set:function(u){location.assign(u);}});
    Object.defineProperty(document,'defaultView',{get:function(){return win;}});
    ['parent','top','frames','length'].forEach(function(n){Object.defineProperty(win,n,{get:function(){return dom(n,null);}});});
    win.window=win.self=win;win.document=document;win.Node=win.Element=win.HTMLElement=Node;win.Event=Event;
    /* Per-realm identity; keep the object extensible for site polyfills. */
    function Navigator(){throw TypeError('Illegal constructor');}
    var nav=Object.create(Navigator.prototype), userAgent=dom('userAgent');
    var language=dom('language'), languages=Object.freeze([language]);
    var identity={appCodeName:'Mozilla',appName:'Netscape',
        appVersion:userAgent.slice(userAgent.indexOf('/')+1),userAgent:userAgent,
        platform:'BE-300',product:'Gecko',productSub:'20100101',vendor:'',vendorSub:'',
        language:language,languages:languages,cookieEnabled:true,
        hardwareConcurrency:1,maxTouchPoints:1};
    Object.keys(identity).forEach(function(key){Object.defineProperty(Navigator.prototype,key,
        {get:function(){return identity[key];},enumerable:true,configurable:true});});
    Object.defineProperty(Navigator.prototype,Symbol.toStringTag,{value:'Navigator',configurable:true});
    win.Navigator=Navigator;
    ['navigator','clientInformation'].forEach(function(key){Object.defineProperty(win,key,
        {get:function(){return nav;},enumerable:true,configurable:true});});
    var performance={now:function(){return dom('performanceNow');},
        toJSON:function(){return {timeOrigin:this.timeOrigin};}};
    Object.defineProperty(performance,'timeOrigin',{value:dom('timeOrigin'),enumerable:true});
    Object.defineProperty(win,'performance',{get:function(){return performance;},configurable:true});
    ['innerWidth','innerHeight'].forEach(function(key){Object.defineProperty(win,key,
        {get:function(){return dom(key);},configurable:true});});
    var screen={width:dom('screenWidth'),height:dom('screenHeight'),
        availWidth:dom('screenWidth'),availHeight:dom('availableHeight'),colorDepth:16,pixelDepth:16};
    Object.keys(screen).forEach(function(key){Object.defineProperty(screen,key,{writable:false});});
    Object.defineProperty(win,'screen',{get:function(){return screen;},configurable:true});
    Object.defineProperty(win,'devicePixelRatio',{value:0.75,configurable:true});
    Object.defineProperty(win,'outerWidth',{get:function(){return screen.width;},configurable:true});
    Object.defineProperty(win,'outerHeight',{get:function(){return screen.height;},configurable:true});
    win.console={log:function(){dom('log',null,Array.prototype.join.call(arguments,' '));}};
    ['warn','error','info','debug','trace','dir','dirxml','table','group','groupCollapsed','assert'].forEach(function(k){
        win.console[k]=k==='assert'?function(ok){if(!ok)win.console.log.apply(win.console,Array.prototype.slice.call(arguments,1));}:function(){win.console.log.apply(win.console,arguments);};});
    ['groupEnd','time','timeEnd','timeLog','count','countReset','clear','profile','profileEnd'].forEach(function(k){win.console[k]=function(){};});
    win.alert=function(s){console.log(s);};
    /* No dialogs yet: a question is answered "OK", a prompt is cancelled. */
    win.confirm=function(s){console.log(s);return true;};
    win.prompt=function(s){console.log(s);return null;};
    win.print=win.focus=win.blur=win.stop=win.close=function(){};
    win.open=function(u){if(u)location.assign(String(u));return null;};
    win.getSelection=document.getSelection=function(){return {rangeCount:0,isCollapsed:true,type:'None',toString:function(){return '';},removeAllRanges:function(){},addRange:function(){},getRangeAt:function(){throw RangeError('No selection');}};};
    win.queueMicrotask=function(fn){Promise.resolve().then(fn);};
    win.requestIdleCallback=function(fn){return win.setTimeout(function(){fn({didTimeout:false,timeRemaining:function(){return 10;}});},1);};
    win.cancelIdleCallback=function(id){win.clearTimeout(id);};
    win.matchMedia=function(q){q=String(q);var m={matches:dom('matchMedia',null,q),media:q,onchange:null};
        m.addListener=m.removeListener=m.addEventListener=m.removeEventListener=function(){};m.dispatchEvent=function(){return true;};return m;};
    /* Scrolling: the document is the only scroller. */
    ['scrollY','pageYOffset'].forEach(function(k){Object.defineProperty(win,k,{get:function(){return dom('scrollY');},configurable:true});});
    ['scrollX','pageXOffset'].forEach(function(k){Object.defineProperty(win,k,{value:0,configurable:true});});
    win.scrollTo=win.scroll=function(x,y){if(x&&typeof x==='object')y=x.top;dom('scrollTo',null,Number(y)||0);};
    win.scrollBy=function(x,y){if(x&&typeof x==='object')y=x.top;dom('scrollTo',null,dom('scrollY')+(Number(y)||0));};
    proto.scrollIntoView=function(){var r=dom('rect',this);if(r)dom('scrollTo',null,dom('scrollY')+r[1]);};
    proto.scrollIntoViewIfNeeded=proto.scrollIntoView;
    function box(k,i){prop(k,function(){var r=dom('rect',this)||[0,0,0,0];return i<2?Math.round(r[i]+(i?dom('scrollY'):0)):Math.round(r[i]);});}
    box('offsetLeft',0);box('offsetTop',1);box('offsetWidth',2);box('offsetHeight',3);box('clientWidth',2);box('clientHeight',3);box('scrollWidth',2);
    prop('scrollHeight',function(){return this===document.documentElement||this===document.body?dom('contentHeight'):(dom('rect',this)||[0,0,0,0])[3];});
    prop('offsetParent',function(){return this===document.body?null:document.body;});
    prop('clientLeft',function(){return 0;});prop('clientTop',function(){return 0;});
    prop('scrollTop',function(){return this===document.documentElement||this===document.body?dom('scrollY'):0;},
         function(v){if(this===document.documentElement||this===document.body)dom('scrollTo',null,Number(v)||0);});
    prop('scrollLeft',function(){return 0;},function(){});
    proto.getClientRects=function(){return [this.getBoundingClientRect()];};
    /* history: back and forward belong to the browser's own controls; the
       state calls are kept so a router does not throw. */
    var hstate=null;
    win.history={length:1,scrollRestoration:'auto',back:function(){},forward:function(){},go:function(n){if(!n)location.reload();},
        pushState:function(s){hstate=s;},replaceState:function(s){hstate=s;}};
    Object.defineProperty(win.history,'state',{get:function(){return hstate;}});
    win.Image=function(width,height){var image=document.createElement('img');
        if(width!==undefined)image.width=width;if(height!==undefined)image.height=height;return image;};
    win.Image.prototype=proto;
    ['complete','naturalWidth','naturalHeight'].forEach(function(name){prop(name,function(){return dom(name,this);});});
    var base64='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    function invalidBase64(){var e=new Error('Invalid character');e.name='InvalidCharacterError';throw e;}
    win.atob=function(value){
        if(!arguments.length)throw TypeError('atob requires an argument');
        var s=String(value).replace(/[\t\n\f\r ]/g,''),out='';
        if(s.length%4===0)s=s.replace(/==?$/,'');
        if(s.length%4===1||/[^A-Za-z0-9+/]/.test(s))invalidBase64();
        for(var i=0;i<s.length;i+=4){var a=base64.indexOf(s[i]),b=base64.indexOf(s[i+1]);
            out+=String.fromCharCode((a<<2)|(b>>4));
            if(i+2<s.length){var c=base64.indexOf(s[i+2]);out+=String.fromCharCode(((b&15)<<4)|(c>>2));
                if(i+3<s.length)out+=String.fromCharCode(((c&3)<<6)|base64.indexOf(s[i+3]));}}
        return out;
    };
    win.btoa=function(value){
        if(!arguments.length)throw TypeError('btoa requires an argument');
        var s=String(value),out='';
        for(var i=0;i<s.length;i+=3){var a=s.charCodeAt(i),b=i+1<s.length?s.charCodeAt(i+1):0,c=i+2<s.length?s.charCodeAt(i+2):0;
            if(a>255||b>255||c>255)invalidBase64();
            out+=base64[a>>2]+base64[((a&3)<<4)|(b>>4)]+(i+1<s.length?base64[((b&15)<<2)|(c>>6)]:'=')+(i+2<s.length?base64[c&63]:'=');}
        return out;
    };
    win.setTimeout=function(fn,delay){if(typeof fn==='string'){var src=fn;fn=function(){(0,win.eval)(src);};}if(typeof fn!=='function')throw TypeError('Timer callback must be a function');if(timers.length>=32)throw Error('Too many timers');var id=nextTimer++,args=Array.prototype.slice.call(arguments,2);timers.push({id:id,fn:fn,args:args,due:dom('now')+Math.max(4,Math.min(2147483647,Number(delay)||0)),repeat:0});return id;};
    win.setInterval=function(fn,delay){var id=win.setTimeout.apply(win,arguments);timers[timers.length-1].repeat=Math.max(4,Number(delay)||0);return id;};
    win.clearTimeout=win.clearInterval=function(id){timers=timers.filter(function(t){return t.id!==id;});};
    function tick(){webTick();var now=dom('now'), copy=timers.slice(),count=0;for(var i=0;i<copy.length&&count<4;i++){var t=copy[i];if(((now-t.due)|0)>=0&&timers.indexOf(t)>=0){if(t.repeat)t.due=now+t.repeat;else win.clearTimeout(t.id);count++;try{t.fn.apply(win,t.args);}catch(err){console.error(err);}}}}
    /* Content policy controls eval as well as classic scripts. Native bindings
       compile permitted inline event handlers without exposing a bypass. */
    if(!dom('evalAllowed')){var denied=function(){throw Error('eval blocked by Content-Security-Policy');};
        Object.defineProperty(Object.getPrototypeOf(function(){}),'constructor',{value:denied});
        Object.defineProperty(Object.getPrototypeOf(function*(){}),'constructor',{value:denied});
        Object.defineProperty(Object.getPrototypeOf(async function(){}),'constructor',{value:denied});
        Object.defineProperty(Object.getPrototypeOf(async function*(){}),'constructor',{value:denied});
        win.eval=win.Function=denied;}
    /* WEB_APIS */
    /* Glaze has no browser workers or shared-memory threading. */
    delete win.SharedArrayBuffer;delete win.Atomics;
    delete win.__dom;
    return {proto:proto,dispatch:dispatch,tick:tick};
})(__dom)
