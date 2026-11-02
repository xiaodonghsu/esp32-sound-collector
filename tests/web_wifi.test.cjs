const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../main/web_config.c'), 'utf8');
const literals = source.slice(source.indexOf('static const char PAGE[]'), source.indexOf('static const char B64[]'));
const html = [...literals.matchAll(/"(?:\\.|[^"\\])*"/g)].map(m => JSON.parse(m[0])).join('');
const script = html.split('<script>')[1].split('</script>')[0].replace(/init\(\)$/, '');
function setup(fetcher) {
    const elements = {};
    const requests = [];
    const timers = [];
    const makeElement = () => ({value:'',disabled:false,textContent:'',children:[],
        replaceChildren(){this.children=[];}, appendChild(child){this.children.push(child);}});
    const context = vm.createContext({TextEncoder,
        setTimeout: callback => {timers.push(callback); return timers.length;}, clearTimeout:()=>{},
        document: {getElementById(id) {return elements[id] ||= makeElement();},createElement:makeElement,querySelectorAll:()=>[]},
        fetch: async (url, options) => {requests.push({url, options}); return fetcher(url, options);},
    });
    vm.runInContext(script, context);
    return {context, elements, requests, timers, get: id => context.document.getElementById(id)};
}
const response = data => ({ok:true,json:async()=>data});
test('Wi-Fi page supports scanning alongside manual SSID input', () => {
    assert.match(html, /<input id=ssid required/);
    assert.match(html, /onclick=scan/);
    assert.match(html, /<select id=networks onchange=chooseNetwork/);
    assert.doesNotMatch(html, /<select id=ssid/);
});
test('warning appears before response, duplicate submit is suppressed and SSID spaces retained', async () => {
    let finish;
    const s=setup(()=>new Promise(resolve=>{finish=resolve;}));
    s.get('ssid').value=' Office ';
    s.get('wpass').value='secret';
    const pending=s.context.saveWifi();
    assert.match(s.get('wifiStatus').textContent,/Wi-Fi 可能中断.*重新连接/);
    await s.context.saveWifi();
    assert.equal(s.requests.length,1);
    assert.deepEqual(JSON.parse(s.requests[0].options.body),{ssid:' Office ',password:'secret'});
    finish(response({accepted:true})); await pending;
    assert.match(s.get('wifiStatus').textContent,/已受理/);
    assert.equal(s.get('wpass').value,'');
    assert.equal(s.get('wifisave').disabled,true);
});
test('reentry shows saved failure as text and a later success clears it', async () => {
    let result={state:'fail',ssid:'<Office>',message:'认证或握手失败（Wi-Fi 原因码 202）'};
    const s=setup(()=>response(result));
    await s.context.loadWifi();
    assert.equal(s.requests[0].url,'/api/wifi');
    assert.match(s.get('wifiStatus').textContent, /<Office>.*认证或握手失败/);
    assert.equal(s.get('wifiStatus').className,'bad');
    result={state:'success',ssid:'Office'};
    await s.context.loadWifi();
    assert.match(s.get('wifiStatus').textContent,/已验证并保存/);
    assert.doesNotMatch(s.get('wifiStatus').textContent,/失败/);
    assert.equal(s.get('wifiStatus').className,'ok');
});
test('pending status blocks resubmission; result loading preserves manual edits', async () => {
    const s=setup(()=>response({state:'pending',ssid:'Old'}));
    s.get('ssid').value='New'; await s.context.loadWifi();
    assert.equal(s.get('ssid').value,'New');
    assert.equal(s.get('wifisave').disabled,true);
    assert.match(s.get('wifiStatus').textContent,/正在验证/);
});
test('network response loss is not reported as definitive connection failure', async () => {
    const s=setup(()=>{throw Error('Failed to fetch');});
    s.get('ssid').value='Office'; await s.context.saveWifi();
    assert.match(s.get('wifiStatus').textContent,/提交响应未确认.*确认结果/);
});
test('byte limits reject oversized UTF-8 SSIDs, allow open networks', async () => {
    const s=setup(()=>response({accepted:true}));
    s.get('ssid').value='中'.repeat(11); await s.context.saveWifi();
    assert.equal(s.requests.length,0);
    s.get('ssid').value='Open'; await s.context.saveWifi();
    assert.equal(JSON.parse(s.requests[0].options.body).password,'');
});

test('scanning populates choices without overwriting manually entered SSID or password', async () => {
    const s=setup((url,options)=>response(options ? {accepted:true} : {state:'success',networks:[
        {ssid:'<Office>',band:'5 GHz',channel:36,frequency_mhz:5180,rssi:-40},
        {ssid:'Office',band:'2.4 GHz',channel:1,frequency_mhz:2412,rssi:-50}
    ]}));
    s.get('ssid').value='Manual'; s.get('wpass').value='secret';
    await s.context.scan();
    assert.equal(s.requests[0].url,'/api/scan');
    assert.equal(s.requests[0].options.method,'POST');
    assert.equal(s.requests[1].options,undefined);
    assert.equal(s.get('networks').children.length,3);
    assert.match(s.get('networks').children[1].textContent, /<Office>.*5 GHz.*36.*5180.*-40/);
    assert.equal(s.get('ssid').value,'Manual');
    s.get('networks').value='<Office>'; s.context.chooseNetwork();
    assert.equal(s.get('ssid').value,'<Office>');
    assert.equal(s.get('wpass').value,'secret');
    s.get('ssid').value='Edited';
    assert.equal(s.get('ssid').value,'Edited');
});
test('pending scans poll read-only status and cached results can be read after reconnect', async () => {
    let data={state:'pending',networks:[]};
    const s=setup(()=>response(data));
    await s.context.loadScan();
    assert.equal(s.get('scanbutton').disabled,true);
    assert.equal(s.timers.length,1);
    data={state:'success',networks:[]};
    await s.timers[0]();
    assert.equal(s.get('scanbutton').disabled,false);
    assert.match(s.get('scanStatus').textContent,/未发现网络/);
    assert.ok(s.requests.every(r=>r.options===undefined));
});
test('Wi-Fi page entry reads status without starting a scan', async () => {
    const s=setup(url=>response(url==='/api/scan'?{state:'idle',networks:[]}:{state:'fail',ssid:'Office',message:'失败原因'}));
    s.context.show(1); await new Promise(resolve=>setImmediate(resolve));
    assert.ok(s.requests.some(r=>r.url==='/api/scan'));
    assert.ok(s.requests.every(r=>r.options===undefined));
    assert.match(s.get('wifiStatus').textContent,/失败原因/);
});
test('scan failure or connection loss preserves manual entry and allows retry', async () => {
    for (const fail of [false,true]) {
        const s=setup(()=>{if(fail)throw Error('offline'); return response({state:'fail',message:'扫描失败，请手动输入 SSID'});});
        s.get('ssid').value='Manual';
        await s.context.loadScan();
        assert.equal(s.get('ssid').value,'Manual');
        assert.equal(s.get('scanbutton').disabled,false);
        assert.match(s.get('scanStatus').textContent,/手动输入 SSID/);
    }
});
