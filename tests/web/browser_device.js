/* Real MIPS/lwIP/flash exercise, served by browser_transport.py's fixture. */
Promise.resolve().then(function(){
    localStorage.setItem('device','persistent');sessionStorage.setItem('device','session');
    if(localStorage.getItem('device')!=='persistent'||sessionStorage.getItem('device')!=='session')throw Error('storage');
    console.log('browser network APIs: storage PASS');
    return fetch('/api-json');
}).then(function(r){return r.json();}).then(function(data){
    if(data.text!=='caf\u00e9 \ud83d\ude00')throw Error('JSON UTF8');
    console.log('browser network APIs: fetch PASS');
    return fetch('/api-echo',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"device":"BE-300"}'});
}).then(function(r){return r.json();}).then(function(data){
    if(data.device!=='BE-300')throw Error('POST');console.log('browser network APIs: POST PASS');
    return import('./module/a.js');
}).then(function(m){
    if(m.answer!==42)throw Error('network modules');console.log('browser network APIs: modules PASS');
    document.getElementById('result').textContent='Network APIs: ready';
},function(e){console.error('browser network APIs: FAIL '+e);document.getElementById('result').textContent=String(e);});
