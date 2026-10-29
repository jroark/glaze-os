var fetchDone=false,fetchError='',fetchChecks=0;
function netCheck(c,m){if(!c)throw Error(m);fetchChecks++;}
function mustReject(p,name){return p.then(function(){throw Error('Request unexpectedly succeeded');},function(e){netCheck(!name||e.name===name,'Expected '+name+', got '+e.name);});}
var foreign=location.origin.replace('127.0.0.1','localhost');
Promise.resolve().then(function(){return fetch('/api-json');}).then(function(r){
    netCheck(r.status===200&&r.ok&&r.headers.get('content-type').indexOf('application/json')===0,'response metadata');
    var copy=r.clone();return r.json().then(function(data){netCheck(data.text==='caf\u00e9 \ud83d\ude00','response UTF8');return mustReject(r.text(),'TypeError');}).then(function(){return copy.arrayBuffer();}).then(function(b){netCheck(b.byteLength>10,'binary body');});
}).then(function(){return fetch('/api-echo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:'\u00e9'})});}).then(function(r){return r.json();}).then(function(data){
    netCheck(data.text==='\u00e9','same-origin JSON POST');return fetch('/api-echo',{method:'POST',body:new URLSearchParams({q:'a b'})});
}).then(function(r){return r.text();}).then(function(text){netCheck(text==='q=a+b','form POST');return fetch('/missing');}).then(function(r){netCheck(r.status===404&&!r.ok,'HTTP errors resolve');return fetch('/api-redirect');}).then(function(r){netCheck(r.redirected&&r.url.indexOf('/api-json')>0,'same-origin redirect');return fetch(foreign+'/api-cors');}).then(function(r){
    netCheck(r.type==='cors','CORS response');return r.json();
}).then(function(data){netCheck(data.origin===location.origin,'Origin header');return fetch(foreign+'/api-cors-credentials',{credentials:'include'});}).then(function(r){return r.json();}).then(function(data){
    netCheck(data.origin===location.origin,'credentialed CORS exact origin');return mustReject(fetch(foreign+'/api-cors',{credentials:'include'}),'TypeError');
}).then(function(){return mustReject(fetch(foreign+'/api-denied'),'TypeError');})
.then(function(){return mustReject(fetch(foreign+'/api-wrong-origin'),'TypeError');})
.then(function(){return mustReject(fetch(foreign+'/api-duplicate-origin'),'TypeError');})
.then(function(){return mustReject(fetch(foreign+'/api-cors',{mode:'same-origin'}),'TypeError');})
.then(function(){return mustReject(fetch(foreign+'/api-no-preflight',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'}),'TypeError');})
.then(function(){return mustReject(fetch('/api-cross-redirect'),'TypeError');})
.then(function(){return mustReject(fetch('/api-large'),'TypeError');})
.then(function(){return mustReject(fetch('file:///private'),'TypeError');})
.then(function(){return mustReject(fetch('/api-json',{headers:{'Authorization':'secret'}}),'TypeError');})
.then(function(){var c=new AbortController();c.abort();return mustReject(fetch('/api-slow',{signal:c.signal}),'AbortError');})
.then(function(){var c=new AbortController(),p=fetch('/api-slow',{signal:c.signal});setTimeout(function(){c.abort();},10);return mustReject(p,'AbortError');})
.then(function(){var controllers=[],jobs=[];for(var i=0;i<8;i++){var c=new AbortController();controllers.push(c);jobs.push(mustReject(fetch('/api-slow',{signal:c.signal}),'AbortError'));}return mustReject(fetch('/api-json'),'TypeError').then(function(){controllers.forEach(function(c){c.abort();});return Promise.all(jobs);});})
.then(function(){fetchDone=true;document.title='fetch PASS';},function(e){fetchDone=true;fetchError=String(e);document.title='fetch FAIL';console.error(e);});
