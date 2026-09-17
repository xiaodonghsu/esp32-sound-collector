// Exercise the actual embedded page script without a device or browser.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../main/web_config.c'), 'utf8');
const literals = source.slice(source.indexOf('static const char PAGE[]'), source.indexOf('static const char B64[]'));
const html = [...literals.matchAll(/"(?:\\.|[^"\\])*"/g)].map(m => JSON.parse(m[0])).join('');
const script = html.split('<script>')[1].split('</script>')[0].replace(/init\(\)$/, '');

function setup() {
    const elements = {};
    const requests = [];
    let response = {available: true, max_bytes: 2097152, running: 'ota_0'};
    class XHR {
        constructor() { this.upload = {}; this.headers = {}; requests.push(this); }
        open(method, url) { this.method = method; this.url = url; }
        setRequestHeader(k, v) { this.headers[k] = v; }
        send(body) { this.body = body; }
    }
    const context = vm.createContext({
        document: {getElementById(id) { return elements[id] ||= {files: [], disabled: false, textContent: '', value: 0}; }},
        fetch: async () => ({ok: true, json: async () => response}), XMLHttpRequest: XHR,
    });
    vm.runInContext(script, context);
    return {context, elements, requests, setResponse(r) {response = r;}};
}

test('unavailable OTA disables upload and shows USB migration guidance', async () => {
    const s = setup(); s.setResponse({available: false, max_bytes: 0});
    await s.context.loadOta();
    assert.equal(s.elements.upgrade.disabled, true);
    assert.match(s.elements.otaInfo.textContent, /USB/);
});

test('empty, wrong extension and oversized files do not start an upload', async () => {
    const s = setup(); await s.context.loadOta();
    for (const file of [undefined, {name:'app.bin',size:0}, {name:'app.zip',size:1024}, {name:'app.bin',size:2097153}]) {
        s.context.document.getElementById('firmware').files = file ? [file] : [];
        s.context.uploadFirmware();
        assert.equal(s.requests.length, 0);
    }
});

test('valid upload uses raw binary and custom header; upload completion waits for validation', async () => {
    const s = setup(); await s.context.loadOta();
    const file = {name:'app.bin',size:1024}; s.context.document.getElementById('firmware').files = [file];
    s.context.uploadFirmware(); s.context.uploadFirmware();
    assert.equal(s.requests.length, 1);
    const x = s.requests[0];
    assert.equal(x.method, 'POST'); assert.equal(x.url, '/api/ota');
    assert.equal(x.headers['Content-Type'], 'application/octet-stream');
    assert.equal(x.headers['X-OTA-Upload'], '1'); assert.equal(x.body, file);
    x.upload.onprogress({lengthComputable:true,loaded:1024,total:1024});
    assert.match(s.elements.otaStatus.textContent, /校验/);
    x.status=200; x.responseText='{"ok":true}'; x.onload();
    assert.match(s.elements.otaStatus.textContent, /重启/);
    assert.equal(s.elements.upgrade.disabled, true);
});

test('validation errors, network errors, timeouts and malformed responses permit retry', async () => {
    for (const failure of ['validation', 'network', 'timeout', 'malformed']) {
        const s = setup(); await s.context.loadOta();
        s.context.document.getElementById('firmware').files = [{name:'app.bin',size:1024}]; s.context.uploadFirmware();
        const x = s.requests[0];
        if (failure === 'network') x.onerror();
        else if (failure === 'timeout') x.ontimeout();
        else {x.status=400; x.responseText=failure==='malformed'?'bad':'{"error":"校验失败"}'; x.onload();}
        assert.equal(s.elements.upgrade.disabled, false);
        assert.equal(s.elements.firmware.disabled, false);
        s.context.uploadFirmware(); assert.equal(s.requests.length, 2);
    }
});

