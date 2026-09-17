#include "web_config.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "app_config.h"
#include "device_identity.h"
#include "mqtt_control.h"
#include "wifi_manager.h"

static const char *TAG = "web_config";
static httpd_handle_t s_server;

#define STRINGIFY_VALUE_(value) #value
#define STRINGIFY_VALUE(value) STRINGIFY_VALUE_(value)

static const char PAGE[] =
"<!doctype html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Bestlink Recorder</title><style>body{font-family:system-ui;margin:0;background:#f4f6f8;color:#17212b}main{max-width:680px;margin:auto;padding:24px}"
"h1{font-size:24px}.tabs{display:flex;flex-wrap:wrap;gap:8px;margin:20px 0}.tabs button{flex:1;min-width:100px}.card{background:white;padding:22px;border-radius:12px;box-shadow:0 2px 10px #0001}"
"dl{display:grid;grid-template-columns:minmax(110px,1fr) 2fr;gap:10px;margin:0}dt{color:#526170}dd{margin:0;overflow-wrap:anywhere}h3{border-bottom:1px solid #dfe5eb;padding-bottom:10px}#overviewStatus{color:#526170;font-size:14px}"
"label{display:block;margin:14px 0 5px}input,select,button{box-sizing:border-box;width:100%;padding:11px;border:1px solid #bac3cc;border-radius:7px}"
"button{cursor:pointer;background:#1267d6;color:white;border:0;font-weight:600}.tabs button.off{background:#dfe5eb;color:#344}.page{display:none}.page.on{display:block}"
"#msg{min-height:24px;margin-top:14px}.ok{color:#08783e}.bad{color:#b42318}</style></head><body><main>"
"<h1>Bestlink 4-Mic Recorder</h1><div id=identity></div><div class=tabs><button onclick='show(0)'>设备概览</button><button class=off onclick='show(1)'>Wi-Fi</button><button class=off onclick='show(2)'>MQTT 通信参数</button><button class=off onclick='show(3)'>登录密码</button></div>"
"<section class='card page on'><h2>设备概览</h2><button id=refresh onclick=refreshOverview()>刷新概览</button><p id=overviewStatus role=status>正在读取…</p><h3>设备与固件</h3><dl id=device></dl><h3>存储与内存</h3><dl id=memory></dl><p>内存统计为可分配的堆内存；历史最低可用量从本次启动起统计。</p><h3>STA · 上行 Wi-Fi</h3><dl id=sta></dl><h3>Soft AP · 配置热点</h3><dl id=ap></dl></section>"
"<section class='card page'><h2>Wi-Fi 设置</h2><button onclick=scan()>扫描附近网络</button><label>SSID</label><select id=ssid></select><label>Wi-Fi 密码</label><input id=wpass type=password autocomplete=new-password><button onclick=saveWifi()>验证并保存</button></section>"
"<section class='card page'><h2>MQTT 通信参数</h2><p>保存后自动使用新参数连接，无需重启设备。</p>"
"<form onsubmit='event.preventDefault();saveMqtt()'><label for=host>服务器地址（IP 或域名）</label><input id=host required maxlength=128 autocomplete=off>"
"<label for=port>端口号（1–65535）</label><input id=port type=number required min=1 max=65535 step=1>"
"<label for=user>用户名</label><input id=user maxlength=64 autocomplete=off>"
"<label for=mpass>密码</label><input id=mpass type=password maxlength=64 autocomplete=new-password placeholder='留空保留当前密码'>"
"<p>初始默认：" APP_MQTT_DEFAULT_HOST ":" STRINGIFY_VALUE(APP_MQTT_DEFAULT_PORT) "；用户名 " APP_MQTT_DEFAULT_USERNAME "；密码 " APP_MQTT_DEFAULT_PASSWORD "。已保存的密码不回显。</p>"
"<button id=mqttsave type=submit>保存并应用</button></form></section>"
"<section class='card page'><h2>修改登录密码</h2><label>新密码（至少 6 个字符）</label><input id=newpass type=password autocomplete=new-password><button onclick=savePassword()>修改密码</button></section><div id=msg></div>"
"<script>const $=id=>document.getElementById(id);function show(n){document.querySelectorAll('.page').forEach((e,i)=>e.classList.toggle('on',i===n));document.querySelectorAll('.tabs button').forEach((e,i)=>e.classList.toggle('off',i!==n));if(n===0)refreshOverview();if(n===1&&!$('ssid').options.length)scan()}"
"function note(s,ok=true){$('msg').textContent=s;$('msg').className=ok?'ok':'bad'}async function api(u,o){let r=await fetch(u,o);let j=await r.json().catch(()=>({error:r.statusText}));if(!r.ok)throw Error(j.error||'请求失败');return j}"
"function rows(id,items){$(id).replaceChildren();items.forEach(([k,v])=>{let dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=k;dd.textContent=v==null?'不可用':v;$(id).append(dt,dd)})}"
"function bytes(n){return n==null?'不可用':(n/1024/(n>=1048576?1024:1)).toFixed(2)+(n>=1048576?' MiB':' KiB')+' ('+n+' B)'}"
"function heapRows(name,h){return [['总堆内存',h.total],['当前可用',h.free],['历史最低可用',h.minimum_free],['最大连续块',h.largest_block]].map(([k,v])=>[name+' '+k,bytes(v)])}"
"async function refreshOverview(){if($('refresh').disabled)return;$('refresh').disabled=true;try{let j=await api('/api/overview'),d=j.device,m=j.memory,s=j.sta,a=j.ap;"
"rows('device',[['设备 ID',d.id],['ESP-IDF',d.idf],['固件版本',d.firmware],['芯片',d.chip],['芯片修订',d.revision],['CPU 核心数',d.cores],['运行时间',d.uptime_seconds+' 秒']]);"
"rows('memory',[['Flash 实测容量',bytes(m.flash_bytes)],['Flash 构建配置',m.flash_config],['PSRAM 状态',m.psram_status],['PSRAM 检测容量',bytes(m.psram_bytes)],...heapRows('内部 RAM',m.internal),...heapRows('PSRAM',m.psram)]);"
"rows('sta',[['状态',s.status],['SSID',s.ssid],['MAC',s.mac],['接入点 BSSID',s.bssid],['信号强度',s.rssi==null?null:s.rssi+' dBm'],['信道',s.channel],['IPv4',s.ip],['子网掩码',s.netmask],['网关',s.gateway]]);"
"rows('ap',[['状态',a.enabled?'已启用':'未启用'],['SSID',a.ssid],['MAC',a.mac],['IPv4',a.ip],['子网掩码',a.netmask],['信道',a.channel],['安全模式',a.security],['已连接客户端',a.clients],['客户端上限',a.max_clients]]);$('overviewStatus').textContent='更新于 '+new Date().toLocaleTimeString()}catch(e){$('overviewStatus').textContent='刷新失败，显示内容可能已过期：'+e.message}finally{$('refresh').disabled=false}}"
"async function init(){refreshOverview();try{let j=await api('/api/info');$('identity').textContent='设备 '+j.device_id+' · '+j.ap_ssid;$('host').value=j.mqtt_host;$('port').value=j.mqtt_port;$('user').value=j.mqtt_username}catch(e){note(e.message,false)}}"
"async function scan(){try{note('正在扫描…');let j=await api('/api/scan');$('ssid').innerHTML='';j.networks.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';if(n.ssid===j.current)o.selected=true;$('ssid').appendChild(o)});note('扫描完成')}catch(e){note(e.message,false)}}"
"async function post(u,b){return api(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})}"
"async function saveWifi(){try{note('正在验证连接，最长约 20 秒…');await post('/api/wifi',{ssid:$('ssid').value,password:$('wpass').value});note('Wi-Fi 已验证并保存')}catch(e){note(e.message,false)}}"
"async function saveMqtt(){let b=$('mqttsave');b.disabled=true;try{note('正在保存 MQTT 通信参数…');await post('/api/mqtt',{host:$('host').value.trim(),port:Number($('port').value),username:$('user').value,password:$('mpass').value||null});$('mpass').value='';note('MQTT 参数已保存；Wi-Fi 联网后自动连接服务器')}catch(e){note(e.message,false)}finally{b.disabled=false}}"
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

static void add_mac(cJSON *j, const char *key, const uint8_t *mac)
{
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(j, key, text);
}

static void add_ip_info(cJSON *j, esp_netif_t *netif)
{
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    char text[16];
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    cJSON_AddStringToObject(j, "ip", text);
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.netmask));
    cJSON_AddStringToObject(j, "netmask", text);
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.gw));
    cJSON_AddStringToObject(j, "gateway", text);
}

static void add_heap_info(cJSON *j, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    cJSON_AddNumberToObject(j, "total", info.total_free_bytes + info.total_allocated_bytes);
    cJSON_AddNumberToObject(j, "free", info.total_free_bytes);
    cJSON_AddNumberToObject(j, "minimum_free", info.minimum_free_bytes);
    cJSON_AddNumberToObject(j, "largest_block", info.largest_free_block);
}

static esp_err_t overview_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON *device = cJSON_AddObjectToObject(j, "device");
    cJSON *memory = cJSON_AddObjectToObject(j, "memory");
    cJSON *sta = cJSON_AddObjectToObject(j, "sta");
    cJSON *ap = cJSON_AddObjectToObject(j, "ap");
    cJSON *internal = memory ? cJSON_AddObjectToObject(memory, "internal") : NULL;
    cJSON *psram = memory ? cJSON_AddObjectToObject(memory, "psram") : NULL;
    if (!device || !memory || !sta || !ap || !internal || !psram) {
        cJSON_Delete(j);
        return ESP_ERR_NO_MEM;
    }
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(device, "id", device_identity_id());
    cJSON_AddStringToObject(device, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(device, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(device, "chip", CONFIG_IDF_TARGET);
    char revision[16];
    snprintf(revision, sizeof(revision), "v%u.%u", chip.revision / 100, chip.revision % 100);
    cJSON_AddStringToObject(device, "revision", revision);
    cJSON_AddNumberToObject(device, "cores", chip.cores);
    cJSON_AddNumberToObject(device, "uptime_seconds", esp_timer_get_time() / 1000000);
    uint32_t flash_bytes;
    if (esp_flash_get_physical_size(NULL, &flash_bytes) == ESP_OK)
        cJSON_AddNumberToObject(memory, "flash_bytes", flash_bytes);
    cJSON_AddStringToObject(memory, "flash_config", CONFIG_ESPTOOLPY_FLASHSIZE);
#if CONFIG_SPIRAM
    bool initialized = esp_psram_is_initialized();
    cJSON_AddStringToObject(memory, "psram_status", initialized ? "已初始化" : "初始化失败 / 不可用");
    if (initialized) cJSON_AddNumberToObject(memory, "psram_bytes", esp_psram_get_size());
#else
    cJSON_AddStringToObject(memory, "psram_status", "固件未启用（无法判断物理容量）");
#endif
    add_heap_info(internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    add_heap_info(psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    wifi_ap_record_t connected_ap;
    bool associated = esp_wifi_sta_get_ap_info(&connected_ap) == ESP_OK;
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = associated && wifi_manager_is_connected() && sta_netif && esp_netif_is_netif_up(sta_netif);
    cJSON_AddStringToObject(sta, "status", connected ? "已连接（已获取 IP）" : associated ? "已关联，等待 IP" : "未连接");
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) add_mac(sta, "mac", mac);
    if (associated) {
        char ssid[33];
        memcpy(ssid, connected_ap.ssid, 32);
        ssid[32] = '\0';
        cJSON_AddStringToObject(sta, "ssid", ssid);
        add_mac(sta, "bssid", connected_ap.bssid);
        cJSON_AddNumberToObject(sta, "rssi", connected_ap.rssi);
        cJSON_AddNumberToObject(sta, "channel", connected_ap.primary);
    }
    // Do not expose a retained DHCP address when the station is offline.
    if (connected) add_ip_info(sta, sta_netif);

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    bool enabled = ap_netif && esp_netif_is_netif_up(ap_netif);
    cJSON_AddBoolToObject(ap, "enabled", enabled);
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) add_mac(ap, "mac", mac);
    wifi_config_t config;
    if (esp_wifi_get_config(WIFI_IF_AP, &config) == ESP_OK) {
        char ssid[33];
        memcpy(ssid, config.ap.ssid, 32);
        ssid[32] = '\0';
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "max_clients", config.ap.max_connection);
        cJSON_AddStringToObject(ap, "security", config.ap.authmode == WIFI_AUTH_OPEN ? "开放网络" : "加密网络");
    }
    if (enabled) {
        add_ip_info(ap, ap_netif);
        uint8_t primary;
        wifi_second_chan_t secondary;
        if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK)
            cJSON_AddNumberToObject(ap, "channel", primary);
        wifi_sta_list_t clients;
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK)
            cJSON_AddNumberToObject(ap, "clients", clients.num);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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
    cJSON *password_item = cJSON_GetObjectItemCaseSensitive(j, "password");
    if (password_item && !cJSON_IsNull(password_item) && !cJSON_IsString(password_item)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "MQTT 密码必须为字符串");
    }
    app_config_t current;
    app_config_get(&current);
    if (!password) password = current.mqtt_password;
    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(j, "port");
    int port = cJSON_IsNumber(port_item) ? port_item->valueint : 0;
    esp_err_t err = (port > 0 && port <= 65535 && port_item->valuedouble == port)
                       ? app_config_set_mqtt(host, port, username, password) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "MQTT 参数无效，请检查地址、整数端口及字段长度");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "MQTT 参数保存失败，请重试");
    err = mqtt_control_reload();
    if (err != ESP_OK) return send_error(req, "503 Service Unavailable", "MQTT 参数已保存，但应用失败，请重新保存或重启设备");
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
        {.uri = "/api/overview", .method = HTTP_GET, .handler = overview_get},
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
