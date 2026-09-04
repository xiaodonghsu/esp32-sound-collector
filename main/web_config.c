#include "web_config.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "app_config.h"
#include "device_identity.h"
#include "mqtt_control.h"
#include "wifi_manager.h"

static const char *TAG = "web_config";
static httpd_handle_t s_server;

static const char PAGE[] =
"<!doctype html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Bestlink Recorder</title><style>body{font-family:system-ui;margin:0;background:#f4f6f8;color:#17212b}main{max-width:680px;margin:auto;padding:24px}"
"h1{font-size:24px}.tabs{display:flex;gap:8px;margin:20px 0}.tabs button{flex:1}.card{background:white;padding:22px;border-radius:12px;box-shadow:0 2px 10px #0001}"
"label{display:block;margin:14px 0 5px}input,select,button{box-sizing:border-box;width:100%;padding:11px;border:1px solid #bac3cc;border-radius:7px}"
"button{cursor:pointer;background:#1267d6;color:white;border:0;font-weight:600}.tabs button.off{background:#dfe5eb;color:#344}.page{display:none}.page.on{display:block}"
"#msg{min-height:24px;margin-top:14px}.ok{color:#08783e}.bad{color:#b42318}</style></head><body><main>"
"<h1>Bestlink 4-Mic Recorder</h1><div id=identity></div><div class=tabs><button onclick='show(0)'>Wi-Fi</button><button class=off onclick='show(1)'>MQTT</button><button class=off onclick='show(2)'>登录密码</button></div>"
"<section class='card page on'><h2>Wi-Fi 设置</h2><button onclick=scan()>扫描附近网络</button><label>SSID</label><select id=ssid></select><label>Wi-Fi 密码</label><input id=wpass type=password autocomplete=new-password><button onclick=saveWifi()>验证并保存</button></section>"
"<section class='card page'><h2>MQTT 设置</h2><label>服务器地址</label><input id=host><label>端口</label><input id=port type=number min=1 max=65535><label>用户名</label><input id=user><label>密码</label><input id=mpass type=password><button onclick=saveMqtt()>保存</button></section>"
"<section class='card page'><h2>修改登录密码</h2><label>新密码（至少 6 个字符）</label><input id=newpass type=password autocomplete=new-password><button onclick=savePassword()>修改密码</button></section><div id=msg></div>"
"<script>const $=id=>document.getElementById(id);function show(n){document.querySelectorAll('.page').forEach((e,i)=>e.classList.toggle('on',i===n));document.querySelectorAll('.tabs button').forEach((e,i)=>e.classList.toggle('off',i!==n))}"
"function note(s,ok=true){$('msg').textContent=s;$('msg').className=ok?'ok':'bad'}async function api(u,o){let r=await fetch(u,o);let j=await r.json().catch(()=>({error:r.statusText}));if(!r.ok)throw Error(j.error||'请求失败');return j}"
"async function init(){try{let j=await api('/api/info');$('identity').textContent='设备 '+j.device_id+' · '+j.ap_ssid;$('host').value=j.mqtt_host;$('port').value=j.mqtt_port;$('user').value=j.mqtt_username;await scan()}catch(e){note(e.message,false)}}"
"async function scan(){try{note('正在扫描…');let j=await api('/api/scan');$('ssid').innerHTML='';j.networks.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';if(n.ssid===j.current)o.selected=true;$('ssid').appendChild(o)});note('扫描完成')}catch(e){note(e.message,false)}}"
"async function post(u,b){return api(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})}"
"async function saveWifi(){try{note('正在验证连接，最长约 20 秒…');await post('/api/wifi',{ssid:$('ssid').value,password:$('wpass').value});note('Wi-Fi 已验证并保存')}catch(e){note(e.message,false)}}"
"async function saveMqtt(){try{await post('/api/mqtt',{host:$('host').value,port:Number($('port').value),username:$('user').value,password:$('mpass').value||null});note('MQTT 设置已保存并重新连接')}catch(e){note(e.message,false)}}"
"async function savePassword(){try{await post('/api/password',{password:$('newpass').value});note('密码已修改，下次请求需使用新密码')}catch(e){note(e.message,false)}}init()</script></main></body></html>";

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    size_t p = 0;
    for (size_t i = 0; i < len && p + 4 < out_size; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[p++] = B64[(v >> 18) & 63];
        out[p++] = B64[(v >> 12) & 63];
        out[p++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[p++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[p] = '\0';
}

static bool authorized(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_get(&cfg);
    char plain[APP_WEB_PASS_MAX_LEN + 8];
    snprintf(plain, sizeof(plain), "admin:%s", cfg.web_password);
    char encoded[128];
    base64_encode((const uint8_t *)plain, strlen(plain), encoded, sizeof(encoded));
    char expected[136];
    snprintf(expected, sizeof(expected), "Basic %s", encoded);
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (!len || len >= sizeof(expected)) return false;
    char actual[136];
    if (httpd_req_get_hdr_value_str(req, "Authorization", actual, sizeof(actual)) != ESP_OK) return false;
    return strcmp(actual, expected) == 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    if (authorized(req)) return ESP_OK;
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Bestlink Recorder\"");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authentication required");
    return ESP_FAIL;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json, const char *status)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    if (status) httpd_resp_set_status(req, status);
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    return send_json(req, json, status);
}

static cJSON *receive_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
    char *body = calloc(1, req->content_len + 1);
    if (!body) return NULL;
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) { free(body); return NULL; }
        received += n;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

static const char *json_string(cJSON *json, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t root_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    app_config_t cfg;
    app_config_get(&cfg);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "device_id", device_identity_id());
    cJSON_AddStringToObject(j, "ap_ssid", device_identity_ap_ssid());
    cJSON_AddStringToObject(j, "mqtt_host", cfg.mqtt_host);
    cJSON_AddNumberToObject(j, "mqtt_port", cfg.mqtt_port);
    cJSON_AddStringToObject(j, "mqtt_username", cfg.mqtt_username);
    return send_json(req, j, NULL);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    wifi_ap_record_t *records = NULL;
    uint16_t count = 0;
    if (wifi_manager_scan(&records, &count) != ESP_OK) return send_error(req, "503 Service Unavailable", "Wi-Fi 扫描失败");
    cJSON *j = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(j, "networks");
    for (uint16_t i = 0; i < count; ++i) {
        if (!records[i].ssid[0]) continue;
        bool duplicate = false;
        for (uint16_t k = 0; k < i; ++k) if (!strcmp((char *)records[k].ssid, (char *)records[i].ssid)) duplicate = true;
        if (duplicate) continue;
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", records[i].rssi);
        cJSON_AddItemToArray(networks, n);
    }
    free(records);
    app_config_t cfg;
    app_config_get(&cfg);
    cJSON_AddStringToObject(j, "current", cfg.wifi_ssid);
    return send_json(req, j, NULL);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = receive_json(req);
    if (!j) return send_error(req, "400 Bad Request", "无效 JSON");
    const char *ssid = json_string(j, "ssid");
    const char *password = json_string(j, "password");
    esp_err_t err = wifi_manager_test_and_save(ssid, password, 20000);
    cJSON_Delete(j);
    if (err != ESP_OK) return send_error(req, "400 Bad Request", "无法连接该 Wi-Fi，请检查密码和信号");
    cJSON *ok = cJSON_CreateObject(); cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok, NULL);
}

static esp_err_t mqtt_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = receive_json(req);
    if (!j) return send_error(req, "400 Bad Request", "无效 JSON");
    const char *host = json_string(j, "host");
    const char *username = json_string(j, "username");
    const char *password = json_string(j, "password");
    app_config_t current;
    app_config_get(&current);
    if (!password) password = current.mqtt_password;
    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(j, "port");
    int port = cJSON_IsNumber(port_item) ? port_item->valueint : 0;
    esp_err_t err = (port > 0 && port <= 65535) ? app_config_set_mqtt(host, port, username, password) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (err != ESP_OK) return send_error(req, "400 Bad Request", "MQTT 参数无效");
    mqtt_control_reload();
    cJSON *ok = cJSON_CreateObject(); cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok, NULL);
}

static esp_err_t password_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = receive_json(req);
    if (!j) return send_error(req, "400 Bad Request", "无效 JSON");
    const char *password = json_string(j, "password");
    esp_err_t err = app_config_set_web_password(password);
    cJSON_Delete(j);
    if (err != ESP_OK) return send_error(req, "400 Bad Request", "密码至少 6 个字符且不超过 64 个字符");
    cJSON *ok = cJSON_CreateObject(); cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok, NULL);
}

esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "http server");
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/mqtt", .method = HTTP_POST, .handler = mqtt_post},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_post},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]), TAG, "register uri");
    }
    ESP_LOGI(TAG, "configuration page started");
    return ESP_OK;
}
