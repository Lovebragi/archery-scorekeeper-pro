// main/archery_wifi.c —— Wi-Fi 扫描 / 连接 / 扫码配网 / NTP 对时（实验版）。
//
// 设计要点：
// - 扫描、NTP 都在独立任务里跑，不阻塞界面；
// - 配网用设备热点 + 网页：手机扫二维码自动加入热点，再打开 192.168.4.1；
// - 口令存 NVS（命名空间 archery，键 w_ssid / w_pass），开机自动重连；
// - 时间按 UTC+8 换算成本地墙上时间（2000 起算的秒），交给应用写 NVS。
#include "archery_wifi.h"
#include "archery_name.h"
#include "archery_wifi_store.h"

#include "archery_voice.h"
#include "archery_app.h"
#include "archery_ws_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "archery_clock.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "archery_wifi";
static const char *PROV_AP_SSID = "Archery-Setup";
static const char *PROV_AP_PASS = "archery1234";
static const char *NVS_NS = "archery";
static const char *NVS_SSID = "w_ssid";
static const char *NVS_PASS = "w_pass";

static archery_wifi_view_t s_view;
static archery_wifi_event_cb_t s_cb;
static void *s_cb_user;
static bool s_inited;
static bool s_connected;
static bool s_provisioning;
static bool s_sntp_started;
static bool s_time_ready;
static uint32_t s_synced_time;
static char s_ip[16];
static char s_sta_ssid[ARCHERY_WIFI_SSID_MAX];
static httpd_handle_t s_httpd;
static const archery_history_t *s_history;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;

static void notify(void) {
    if (s_cb) s_cb(s_cb_user);
}

static void copy_ssid(char *dst, const char *src) {
    strncpy(dst, src ? src : "", ARCHERY_WIFI_SSID_MAX - 1);
    dst[ARCHERY_WIFI_SSID_MAX - 1] = '\0';
}

static void set_message(const char *message) {
    strncpy(s_view.message, message ? message : "", sizeof(s_view.message) - 1);
    s_view.message[sizeof(s_view.message) - 1] = '\0';
}

// ---------------------------------------------------------------- NVS 凭据
static bool load_credentials(char *ssid, size_t ssid_size, char *pass, size_t pass_size) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssid_len = ssid_size;
    size_t pass_len = pass_size;
    const bool ok = nvs_get_str(handle, NVS_SSID, ssid, &ssid_len) == ESP_OK &&
                    nvs_get_str(handle, NVS_PASS, pass, &pass_len) == ESP_OK;
    nvs_close(handle);
    return ok && ssid[0] != '\0';
}

static bool s_save_enabled = true;
// 用户主动断开后不再自动重连；下次手动连接时恢复。
static bool s_auto_reconnect = true;
static volatile bool s_retry_cancel;
static volatile bool s_retry_in_connect;
static volatile bool s_ap_stopping;
static volatile bool s_sta_resuming;
static volatile bool s_resume_cancel;
static char s_connect_pass[ARCHERY_WIFI_PASS_SIZE];
static bool s_save_pending;

void archery_wifi_set_save_enabled(bool enabled) { s_save_enabled = enabled; }
bool archery_wifi_save_enabled(void) { return s_save_enabled; }

void archery_wifi_set_auto_reconnect(bool enabled) {
    s_auto_reconnect = enabled;
    if (!enabled) archery_wifi_retry_cancel();
}
bool archery_wifi_auto_reconnect_enabled(void) { return s_auto_reconnect; }

void archery_wifi_forget_credentials(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_erase_key(handle, NVS_SSID);
        (void)nvs_erase_key(handle, NVS_PASS);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
    archery_wifi_store_clear();
}

static void save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_str(handle, NVS_SSID, ssid);
    (void)nvs_set_str(handle, NVS_PASS, pass ? pass : "");
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static bool sta_really_associated(void) {
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool archery_wifi_resume_pending(void) {
    return s_ap_stopping || s_sta_resuming;
}

static void cancel_sta_resume(void) {
    s_resume_cancel = true;
    s_sta_resuming = false;
}

// ---------------------------------------------------------------- 事件回调
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_stopping || s_sta_resuming) {
            // 配网停止/STA 恢复期间由恢复任务统一处理，避免事件覆盖状态。
            notify();
            return;
        }
        s_connected = false;
        s_ip[0] = '\0';
        s_view.sta_connected = false;
        s_view.ip[0] = '\0';
        if (s_view.state == ARCHERY_WIFI_CONNECTING) {
            s_view.state = ARCHERY_WIFI_FAILED;
            set_message("连接失败，请检查密码或信号");
        } else if (s_view.state != ARCHERY_WIFI_AP_READY) {
            s_view.state = ARCHERY_WIFI_IDLE;
            set_message("已断开");
        }
        notify();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        const uint32_t ip = event->ip_info.ip.addr;
        snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", (unsigned)(ip & 0xFF),
                 (unsigned)((ip >> 8) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                 (unsigned)((ip >> 24) & 0xFF));
        s_connected = true;
        strncpy(s_view.ip, s_ip, sizeof(s_view.ip) - 1);
        s_view.ip[sizeof(s_view.ip) - 1] = '\0';
        s_view.sta_connected = true;
        s_view.state = ARCHERY_WIFI_CONNECTED;
        s_sta_resuming = false;
        s_save_pending = true;
        set_message("已连接，可立即对时");
        notify();
        ESP_LOGI(TAG, "已获取 IP: %s", s_ip);
    }
}

// ---------------------------------------------------------------- 扫描
// 阻塞扫描并写入视图；返回扫描到的网络数。
static uint8_t run_scan(void) {
    wifi_scan_config_t config = {0};
    esp_err_t err = esp_wifi_scan_start(&config, true);
    for (int retry = 0; err != ESP_OK && retry < 3; retry++) {
        vTaskDelay(pdMS_TO_TICKS(300));
        err = esp_wifi_scan_start(&config, true);
    }
    uint16_t count = 0;
    wifi_ap_record_t records[ARCHERY_WIFI_AP_MAX];
    if (err == ESP_OK) {
        uint16_t total = 0;
        esp_wifi_scan_get_ap_num(&total);
        count = total > ARCHERY_WIFI_AP_MAX ? ARCHERY_WIFI_AP_MAX : total;
        if (count > 0 && esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
            count = 0;
        }
    }
    s_view.ap_count = (uint8_t)count;
    for (uint8_t i = 0; i < count; i++) {
        copy_ssid(s_view.ssid[i], (const char *)records[i].ssid);
        s_view.rssi[i] = records[i].rssi;
        s_view.secure[i] = records[i].authmode != WIFI_AUTH_OPEN;
    }
    return (uint8_t)count;
}

static void scan_task(void *arg) {
    (void)arg;
    s_view.state = ARCHERY_WIFI_SCANNING;
    set_message("正在扫描…");
    notify();

    const uint8_t count = run_scan();
    if (count == 0) {
        s_view.state = ARCHERY_WIFI_FAILED;
        set_message("没有扫描到网络，请靠近路由器重试");
    } else {
        s_view.state = ARCHERY_WIFI_LIST;
        set_message("");
    }
    notify();
    vTaskDelete(NULL);
}

esp_err_t archery_wifi_scan(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    archery_wifi_retry_cancel();
    cancel_sta_resume();
    if (s_view.state == ARCHERY_WIFI_SCANNING) return ESP_OK;
    if (s_view.state == ARCHERY_WIFI_CONNECTING) (void)esp_wifi_disconnect();
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 连接
esp_err_t archery_wifi_connect(const char *ssid, const char *password) {
    if (!s_inited || !ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!s_retry_in_connect) {
        s_retry_cancel = true;
        cancel_sta_resume();
    }
    s_auto_reconnect = true;
    strncpy(s_connect_pass, password ? password : "", sizeof(s_connect_pass) - 1);
    s_connect_pass[sizeof(s_connect_pass) - 1] = '\0';

    if (s_provisioning) {
        archery_wifi_stop_provisioning();
    }
    s_view.state = ARCHERY_WIFI_CONNECTING;
    copy_ssid(s_sta_ssid, ssid);
    copy_ssid(s_view.ssid[0], ssid);
    set_message("正在连接…");
    notify();

    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, password ? password : "",
            sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_connect();
    return ESP_OK;
}

esp_err_t archery_wifi_connect_index(uint8_t index) {
    if (index >= s_view.ap_count) return ESP_ERR_INVALID_ARG;
    if (s_view.secure[index]) {
        // 加密网络：打开热点，让手机在网页里输入密码。
        return archery_wifi_start_provisioning(s_view.ssid[index]);
    }
    return archery_wifi_connect(s_view.ssid[index], "");
}

static volatile bool s_retry_saved_active;

static void retry_saved_task(void *arg) {
    (void)arg;
    archery_wifi_store_t store;
    if (archery_wifi_store_load(&store)) {
        for (uint8_t i = 0; i < store.count; i++) {
            if (s_connected || s_retry_cancel) break;
            s_retry_in_connect = true;
            (void)archery_wifi_connect(store.items[i].ssid, store.items[i].pass);
            s_retry_in_connect = false;
            for (int tick = 0; tick < 20 && !s_connected; tick++) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (s_retry_cancel || s_view.state == ARCHERY_WIFI_FAILED) break;
            }
        }
    }
    s_retry_saved_active = false;
    vTaskDelete(NULL);
}

void archery_wifi_retry_cancel(void) { s_retry_cancel = true; }
bool archery_wifi_retry_active(void) { return s_retry_saved_active; }

void archery_wifi_retry_saved_async(void) {
    if (s_retry_saved_active || s_connected) return;
    s_retry_cancel = false;
    s_retry_saved_active = true;
    if (xTaskCreate(retry_saved_task, "wifi_retry", 4096, NULL, 3, NULL) != pdPASS) {
        s_retry_saved_active = false;
    }
}

static bool s_suspended_for_sleep;

// 手动浅睡前必须停 Wi-Fi（ESP-IDF 要求）；唤醒后再 start 并重连。
esp_err_t archery_wifi_suspend_for_light_sleep(void) {
    if (!s_inited || s_provisioning) return ESP_ERR_INVALID_STATE;
    s_suspended_for_sleep = true;
    s_connected = false;
    s_ip[0] = '\0';
    s_view.sta_connected = false;
    s_view.ip[0] = '\0';
    s_view.state = ARCHERY_WIFI_IDLE;
    return esp_wifi_stop();
}

esp_err_t archery_wifi_resume_after_light_sleep(void) {
    if (!s_suspended_for_sleep) return ESP_OK;
    s_suspended_for_sleep = false;
    return esp_wifi_start();
}
const char *archery_wifi_current_ssid(void) { return s_sta_ssid; }

esp_err_t archery_wifi_reconnect(void) {
    if (!s_inited || !s_auto_reconnect) return ESP_ERR_INVALID_STATE;
    if (s_connected || s_view.state == ARCHERY_WIFI_CONNECTING) return ESP_OK;
    char ssid[ARCHERY_WIFI_SSID_MAX];
    char pass[65];
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return ESP_ERR_NOT_FOUND;
    }
    return archery_wifi_connect(ssid, pass);
}

esp_err_t archery_wifi_disconnect(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    archery_wifi_retry_cancel();
    cancel_sta_resume();
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    s_connected = false;
    s_ip[0] = '\0';
    s_view.sta_connected = false;
    s_view.ip[0] = '\0';
    s_view.state = ARCHERY_WIFI_IDLE;
    set_message("已断开");
    notify();
    return ESP_OK;
}

void archery_wifi_commit_credentials(void) {
    if (!s_save_pending) return;
    s_save_pending = false;
    if (s_save_enabled) {
        save_credentials(s_sta_ssid, s_connect_pass);
        archery_wifi_store_remember(s_sta_ssid, s_connect_pass);
    }
}

bool archery_wifi_connected(void) { return s_connected; }
bool archery_wifi_provisioning(void) { return s_provisioning; }

// ---------------------------------------------------------------- NTP
static void ntp_task(void *arg) {
    (void)arg;
    s_view.state = ARCHERY_WIFI_CONNECTED;
    set_message("正在对时…");
    notify();

    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "cn.pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
    }
    time_t now = 0;
    for (int i = 0; i < 40; i++) {
        time(&now);
        if (now > 1600000000) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (now > 1600000000) {
        now += 8 * 3600;  // 本地墙上时间（中国时区）
        struct tm tm_now;
        gmtime_r(&now, &tm_now);
        const archery_datetime_t dt = {
            (uint16_t)(tm_now.tm_year + 1900), (uint8_t)(tm_now.tm_mon + 1),
            (uint8_t)tm_now.tm_mday, (uint8_t)tm_now.tm_hour,
            (uint8_t)tm_now.tm_min, (uint8_t)tm_now.tm_sec,
        };
        const uint32_t seconds = archery_datetime_to_seconds(&dt);
        if (seconds != 0) {
            s_synced_time = seconds;
            s_time_ready = true;
            s_view.time_synced = true;
            set_message("对时成功");
        }
    } else {
        set_message("对时失败，请检查网络");
    }
    notify();
    vTaskDelete(NULL);
}

esp_err_t archery_wifi_sync_time(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(ntp_task, "ntp", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool archery_wifi_take_time(uint32_t *seconds) {
    if (!s_time_ready) return false;
    if (seconds) *seconds = s_synced_time;
    s_time_ready = false;
    return true;
}

// ---------------------------------------------------------------- 配网网页
static const char *PROV_HTML =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>射箭计分器配网</title><style>body{font-family:sans-serif;margin:24px;background:#0d1210;color:#f2f1ea}"
    "h2{color:#e8c063}input,button{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border-radius:8px;"
    "border:1px solid #2a352f;background:#16201b;color:#f2f1ea;font-size:16px}button{background:#1d3328;"
    "border-color:#e8c063;color:#e8c063;font-weight:bold}li{padding:10px;margin:4px 0;background:#16201b;"
    "border-radius:8px;list-style:none;cursor:pointer}li:active{outline:1px solid #e8c063}.muted{color:#8a8f87;font-size:14px}"
    ".dl{display:block;text-align:center;padding:12px;margin:8px 0;background:#1d3328;"
    "border:1px solid #e8c063;border-radius:8px;color:#e8c063;text-decoration:none;font-weight:bold}"
    "</style><h2>连接 Wi-Fi</h2><div class=muted>点选下面的网络，输入密码后连接；成功后设备会自动对时。</div>"
    "<a class=dl href='/history.csv'>下载训练记录（CSV，可用 Excel 打开）</a>"
    "<ul id=list><li>扫描中…</li></ul><button onclick=refresh(1)>刷新网络</button>"
    "<input id=ssid placeholder='Wi-Fi 名称' autocapitalize=none autocorrect=off"
    " spellcheck=false autocomplete=off>"
    "<input id=pass type=password placeholder='Wi-Fi 密码' autocapitalize=none"
    " autocorrect=off spellcheck=false autocomplete=off>"
    "<button type=button onclick=\"toggleVisible('pass',this)\">显示</button>"
    "<button onclick=go()>连接</button>"
    "<div class=muted style='margin-top:18px'>豆包 API Key（新版控制台）</div>"
    "<input id=apikey type=password placeholder='API Key' autocapitalize=none"
    " autocorrect=off spellcheck=false autocomplete=off>"
    "<button type=button onclick=\"toggleVisible('apikey',this)\">显示</button>"
    "<button onclick=saveApiKey()>保存 API Key</button>"
    "<div class=muted style='margin-top:18px'>首页名称（1-8 个字符）</div>"
    "<input id=name maxlength=8 placeholder='射箭计分宝' autocapitalize=none"
    " autocorrect=off spellcheck=false autocomplete=off>"
    "<button onclick=saveName()>保存首页名称</button>"
    "<div id=msg class=muted style='margin-top:12px'></div><script>"
    "let ssid=document.getElementById('ssid'),pass=document.getElementById('pass'),"
    "nameInput=document.getElementById('name'),msg=document.getElementById('msg');"
    "fetch('/device-name').then(r=>r.text()).then(t=>{nameInput.value=t});"
    "function toggleVisible(id,btn){const e=document.getElementById(id);const show=e.type==='password';e.type=show?'text':'password';btn.textContent=show?'隐藏':'显示';}"
    "function saveName(){fetch('/device-name',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(nameInput.value)})"
    ".then(r=>r.text()).then(t=>{msg.textContent=t})}"
    "function saveApiKey(){const k=document.getElementById('apikey');"
    "fetch('/voice-api-key',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'key='+encodeURIComponent(k.value)})"
    ".then(r=>r.text()).then(t=>{msg.textContent=t;k.value=''})}"
    "function scan(force){let ctrl=new AbortController();let t=setTimeout(()=>ctrl.abort(),6000);"
    "fetch('/scan'+(force?'?force=1':''),{signal:ctrl.signal}).then(r=>r.json()).then(a=>{clearTimeout(t);"
    "let l=document.getElementById('list');l.innerHTML='';"
    "if(!a.length){l.innerHTML='<li>没有扫描到网络，点“刷新网络”重试</li>';return}"
    "a.forEach(x=>{let li=document.createElement('li');li.textContent=x.s+(x.sec?' [加密]':'')+'  '+x.r+'dBm';"
    "li.onclick=()=>{ssid.value=x.s;};l.appendChild(li)})}).catch(e=>{clearTimeout(t);"
    "document.getElementById('list').innerHTML='<li>扫描超时，点“刷新网络”重试</li>'})}"
    "function refresh(force){msg.textContent='';scan(force)}"
    "scan(0);"
    "function go(){msg.textContent='正在提交…';fetch('/connect',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(ssid.value)+'&pass='+encodeURIComponent(pass.value)})"
    ".then(r=>r.text()).then(t=>{msg.textContent=t;setTimeout(poll,1500)})}"
    "function poll(){fetch('/status').then(r=>r.json()).then(s=>{if(s.connected){msg.textContent='已连接 '+s.ip+'，正在对时…'}else{setTimeout(poll,1500)}})}"
    "</script>";

static void url_decode(char *text) {
    char *write = text;
    for (char *read = text; *read; read++) {
        if (*read == '%' && read[1] && read[2]) {
            char hex[3] = {read[1], read[2], 0};
            *write++ = (char)strtol(hex, NULL, 16);
            read += 2;
        } else if (*read == '+') {
            *write++ = ' ';
        } else {
            *write++ = *read;
        }
    }
    *write = '\0';
}

static esp_err_t http_scan(httpd_req_t *req) {
    // 默认返回开热点前的扫描缓存；手机点"刷新网络"时用 ?force=1 现场扫描。
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        strstr(query, "force=1")) {
        (void)run_scan();
    }
    char json[768];
    size_t used = 0;
    used += (size_t)snprintf(json + used, sizeof(json) - used, "[");
    for (uint8_t i = 0; i < s_view.ap_count && used < sizeof(json) - 80; i++) {
        used += (size_t)snprintf(json + used, sizeof(json) - used,
                                 "%s{\"s\":\"%s\",\"r\":%d,\"sec\":%s}", i ? "," : "",
                                 s_view.ssid[i], (int)s_view.rssi[i],
                                 s_view.secure[i] ? "true" : "false");
    }
    snprintf(json + used, sizeof(json) - used, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t http_diag(httpd_req_t *req) {
    char json[192];
    snprintf(json, sizeof(json),
             "{\"state\":%u,\"aps\":%u,\"connected\":%s,\"ip\":\"%s\",\"prov\":%s}",
             (unsigned)s_view.state, (unsigned)s_view.ap_count,
             s_connected ? "true" : "false", s_connected ? s_ip : "",
             s_provisioning ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t http_status(httpd_req_t *req) {
    char json[128];
    snprintf(json, sizeof(json), "{\"connected\":%s,\"ip\":\"%s\"}",
             s_connected ? "true" : "false", s_connected ? s_ip : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static void provision_connect_task(void *arg) {
    char *payload = (char *)arg;
    char *sep = strchr(payload, '\n');
    const char *pass = "";
    if (sep) {
        *sep = '\0';
        pass = sep + 1;
    }
    archery_wifi_connect(payload, pass);
    free(payload);
    vTaskDelete(NULL);
}

static esp_err_t http_connect(httpd_req_t *req) {
    char body[256];
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[received] = '\0';

    char ssid[64] = {0};
    char pass[96] = {0};
    char *s = strstr(body, "ssid=");
    char *p = strstr(body, "pass=");
    if (s) {
        s += 5;
        char *end = strchr(s, '&');
        if (end) *end = '\0';
        strncpy(ssid, s, sizeof(ssid) - 1);
        url_decode(ssid);
    }
    if (p) {
        p += 5;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        strncpy(pass, p, sizeof(pass) - 1);
        url_decode(pass);
    }
    if (ssid[0] == '\0') return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");

    // 密码留空时优先用已保存的密码，省去重复输入。
    if (pass[0] == '\0' && s_save_enabled) {
        char saved[64];
        if (archery_wifi_store_lookup(ssid, saved, sizeof(saved))) {
            strncpy(pass, saved, sizeof(pass) - 1);
        }
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "已收到，正在连接…");
    // 不能在 HTTP 处理函数里停掉 httpd：交给独立任务。
    char *payload = malloc(sizeof(ssid) + sizeof(pass) + 2);
    if (payload) {
        snprintf(payload, sizeof(ssid) + sizeof(pass) + 2, "%s\n%s", ssid, pass);
        if (xTaskCreate(provision_connect_task, "prov_conn", 4096, payload, 4, NULL) != pdPASS) {
            free(payload);
        }
    }
    return ESP_OK;
}

static esp_err_t http_voice_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, archery_voice_url());
}

static esp_err_t http_voice_set(httpd_req_t *req) {
    char body[192];
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[received] = '\0';

    char url[128] = {0};
    char *value = strstr(body, "url=");
    if (value) {
        value += 4;
        char *end = strchr(value, '&');
        if (end) *end = '\0';
        strncpy(url, value, sizeof(url) - 1);
        url_decode(url);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (archery_voice_save_url(url) != ESP_OK) {
        return httpd_resp_sendstr(req, "地址无效，需要 http://IP:端口/路径");
    }
    return httpd_resp_sendstr(req, url[0] ? "已保存语音地址" : "语音地址已清空");
}

static volatile bool s_voice_verifying;

static void verify_voice_task(void *arg) {
    (void)arg;
    ESP_LOGI("archery_wifi", "voice verify start");
    const bool ok = archery_voice_test_credentials();
    const int code = archery_ws_last_status();
    ESP_LOGI("archery_wifi", "voice verify result=%d http=%d", (int)ok, code);
    if (ok) {
        archery_voice_set_status("语音配置成功");
        archery_app_voice_verified(true);
    } else if (code == 401 || code == 403) {
        archery_voice_set_status("语音 Key 无效");
        archery_app_voice_verified(false);
    } else {
        // 配网页常在热点模式，设备没有外网，不能据此判定 Key 错误。
        archery_voice_set_status("保存成功，但网络不可用");
        archery_app_voice_verified(false);
    }
    archery_app_refresh();
    s_voice_verifying = false;
    vTaskDelete(NULL);
}

void archery_wifi_verify_voice_async(void) {
    archery_voice_prepare_audio();
    if (s_voice_verifying) return;
    s_voice_verifying = true;
    archery_voice_set_status("正在验证语音");
    archery_app_refresh();
    if (xTaskCreate(verify_voice_task, "voice_verify", 8192, NULL, 3, NULL) != pdPASS) {
        s_voice_verifying = false;
        archery_voice_set_status("验证任务创建失败");
        archery_app_refresh();
    }
}

static esp_err_t http_voice_api_key_set(httpd_req_t *req) {
    char body[256];
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[received] = '\0';
    char *k = strstr(body, "key=");
    if (!k) return httpd_resp_sendstr(req, "缺少 key");
    k += 4;
    url_decode(k);
    const esp_err_t err = archery_voice_save_api_key(k);
    if (err != ESP_OK) {
        archery_voice_set_status("语音配置保存失败");
        archery_voice_play_config_failed();
        archery_app_refresh();
        return httpd_resp_sendstr(req, "保存失败");
    }
    archery_voice_set_status("语音已保存");
    archery_app_refresh();
    return httpd_resp_sendstr(req, "API Key 已保存");
}

static esp_err_t http_voice_creds_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, archery_voice_appid());
}

static esp_err_t http_voice_creds_set(httpd_req_t *req) {
    char body[384];
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[received] = '\0';

    char appid[32] = {0};
    char token[96] = {0};
    char *a = strstr(body, "appid=");
    char *t = strstr(body, "token=");
    if (a) {
        a += 6;
        char *end = strchr(a, '&');
        if (end) *end = '\0';
        strncpy(appid, a, sizeof(appid) - 1);
        url_decode(appid);
    }
    if (t) {
        t += 6;
        char *end = strchr(t, '&');
        if (end) *end = '\0';
        strncpy(token, t, sizeof(token) - 1);
        url_decode(token);
    }
    if (token[0] == '\0') {
        strncpy(token, archery_voice_token(), sizeof(token) - 1);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (appid[0] == '\0' || token[0] == '\0') {
        return httpd_resp_sendstr(req, "需要 AppID 和 Access Token");
    }
    if (archery_voice_save_credentials(appid, token) != ESP_OK) {
        return httpd_resp_sendstr(req, "保存失败");
    }
    return httpd_resp_sendstr(req, "已保存直连凭据");
}

static esp_err_t http_redirect(httpd_req_t *req) {
    (void)req;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "redirect");
}

static esp_err_t http_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, PROV_HTML);
}

static esp_err_t http_history_csv(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"archery-history.csv\"");
    // Excel 识别 UTF-8 需要 BOM。
    httpd_resp_send_chunk(req, "\xEF\xBB\xBF", 3);
    const char *header = archery_record_csv_header();
    httpd_resp_send_chunk(req, header, strlen(header));

    char line[256];
    uint16_t count = 0;
    if (s_history) {
        count = archery_history_visible_count(s_history, ARCHERY_HISTORY_FILTER_ALL, 0);
    }
    for (uint16_t i = 0; i < count; i++) {
        const archery_record_t *record =
            archery_history_visible_record(s_history, ARCHERY_HISTORY_FILTER_ALL, 0, i);
        if (!record) continue;
        const size_t length =
            archery_record_csv_row(record, (uint16_t)(i + 1u), line, sizeof(line));
        if (length > 0) httpd_resp_send_chunk(req, line, length);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_device_name_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, archery_name_get());
}

static esp_err_t http_device_name_set(httpd_req_t *req) {
    char body[128];
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[received] = '\0';
    const char *value = strstr(body, "name=");
    if (!value) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no name");
    value += 5;
    char name[64];
    snprintf(name, sizeof(name), "%s", value);
    url_decode(name);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (!archery_name_valid(name, strlen(name))) {
        return httpd_resp_sendstr(req, "名称需 1-8 个字符");
    }
    if (!archery_name_save(name)) return httpd_resp_sendstr(req, "保存失败");
    return httpd_resp_sendstr(req, "已保存");
}

static void start_httpd(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 6144;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败");
        s_httpd = NULL;
        return;
    }
    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = http_index};
    const httpd_uri_t scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = http_scan};
    const httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET,
                                    .handler = http_status};
    const httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST,
                                     .handler = http_connect};
    const httpd_uri_t diag_uri = {.uri = "/diag", .method = HTTP_GET, .handler = http_diag};
    const httpd_uri_t csv_uri = {.uri = "/history.csv", .method = HTTP_GET,
                                 .handler = http_history_csv};
    const httpd_uri_t voice_get_uri = {.uri = "/voice", .method = HTTP_GET,
                                       .handler = http_voice_get};
    const httpd_uri_t voice_set_uri = {.uri = "/voice-url", .method = HTTP_POST,
                                       .handler = http_voice_set};
    const httpd_uri_t creds_get_uri = {.uri = "/voice-creds", .method = HTTP_GET,
                                       .handler = http_voice_creds_get};
    const httpd_uri_t creds_set_uri = {.uri = "/voice-creds", .method = HTTP_POST,
                                       .handler = http_voice_creds_set};
    const httpd_uri_t api_key_uri = {.uri = "/voice-api-key", .method = HTTP_POST,
                                     .handler = http_voice_api_key_set};
    const httpd_uri_t name_get_uri = {.uri = "/device-name", .method = HTTP_GET,
                                       .handler = http_device_name_get};
    const httpd_uri_t name_set_uri = {.uri = "/device-name", .method = HTTP_POST,
                                       .handler = http_device_name_set};
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &name_get_uri);
    httpd_register_uri_handler(s_httpd, &name_set_uri);
    httpd_register_uri_handler(s_httpd, &scan_uri);
    httpd_register_uri_handler(s_httpd, &status_uri);
    httpd_register_uri_handler(s_httpd, &connect_uri);
    httpd_register_uri_handler(s_httpd, &diag_uri);
    httpd_register_uri_handler(s_httpd, &csv_uri);
    httpd_register_uri_handler(s_httpd, &voice_get_uri);
    httpd_register_uri_handler(s_httpd, &voice_set_uri);
    httpd_register_uri_handler(s_httpd, &creds_get_uri);
    httpd_register_uri_handler(s_httpd, &creds_set_uri);
    httpd_register_uri_handler(s_httpd, &api_key_uri);
    // iOS/Android 的联网探测：跳转到配网页，手机才会自动弹出登录页。
    static const char *probe_paths[] = {"/hotspot-detect.html", "/generate_204",
                                        "/connecttest.txt", "/ncsi.txt"};
    for (size_t i = 0; i < sizeof(probe_paths) / sizeof(probe_paths[0]); i++) {
        httpd_uri_t probe = {.uri = probe_paths[i], .method = HTTP_GET,
                             .handler = http_redirect};
        httpd_register_uri_handler(s_httpd, &probe);
    }
}

// 极简 DNS：把所有查询都指向设备热点地址，便于手机自动弹配网页。
static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t buffer[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from,
                                 &from_len);
        if (len < 12) continue;
        uint8_t reply[512];
        memcpy(reply, buffer, (size_t)len);
        reply[2] = 0x81;  // QR=1
        reply[3] = 0x80;  // RA=1
        reply[6] = 0x00;
        reply[7] = 0x01;  // ANCOUNT=1
        int pos = len;
        reply[pos++] = 0xC0;
        reply[pos++] = 0x0C;
        reply[pos++] = 0x00;
        reply[pos++] = 0x01;  // A
        reply[pos++] = 0x00;
        reply[pos++] = 0x01;  // IN
        reply[pos++] = 0x00;
        reply[pos++] = 0x00;
        reply[pos++] = 0x00;
        reply[pos++] = 60;
        reply[pos++] = 0x00;
        reply[pos++] = 0x04;
        reply[pos++] = 192;
        reply[pos++] = 168;
        reply[pos++] = 4;
        reply[pos++] = 1;
        sendto(sock, reply, (size_t)pos, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

esp_err_t archery_wifi_start_provisioning(const char *preset_ssid) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    archery_wifi_retry_cancel();
    cancel_sta_resume();
    if (s_provisioning) return ESP_OK;

    // 先在 STA 模式扫描一次；热点与扫描同时进行会打断手机连接。
    (void)run_scan();
    s_view.state = ARCHERY_WIFI_AP_READY;
    if (preset_ssid && preset_ssid[0]) {
        copy_ssid(s_view.ssid[0], preset_ssid);
        s_view.selected = 0;
    }
    set_message("手机扫码配网");

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, PROV_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(PROV_AP_SSID);
    // 配网热点只存在几分钟：用开放网络，手机点一下就能连，不必输密码。
    // 这比要求用户抄 12 位密码（或依赖微信扫码入网）现实得多。
    ap.ap.password[0] = '\0';
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    start_httpd();
    s_dns_run = true;
    if (xTaskCreate(dns_task, "wifi_dns", 3072, NULL, 3, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
    }
    s_provisioning = true;
    notify();
    ESP_LOGI(TAG, "配网热点已开启: %s / %s", PROV_AP_SSID, PROV_AP_PASS);
    return ESP_OK;
}

static void sta_resume_task(void *arg) {
    (void)arg;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !s_resume_cancel; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_resume_cancel) break;
        if (sta_really_associated()) { ok = true; break; }
        (void)esp_wifi_connect();
        for (int tick = 0; tick < 20 && !s_resume_cancel; tick++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (sta_really_associated()) { ok = true; break; }
        }
        if (ok) break;
    }
    if (s_resume_cancel) {
        // 用户已手动接管，保持当前状态。
    } else if (ok) {
        if (s_connected) s_view.state = ARCHERY_WIFI_CONNECTED;
        set_message("已恢复连接");
    } else {
        s_connected = false;
        s_view.sta_connected = false;
        s_ip[0] = '\0';
        s_view.ip[0] = '\0';
        s_view.state = ARCHERY_WIFI_IDLE;
        set_message("恢复连接失败");
    }
    s_sta_resuming = false;
    notify();
    vTaskDelete(NULL);
}

esp_err_t archery_wifi_stop_provisioning(void) {
    if (!s_provisioning) return ESP_OK;

    // 先进入“停止中”，抑制事件回调的普通断线分支，并取消自动重连。
    s_ap_stopping = true;
    archery_wifi_retry_cancel();
    const bool was_associated = sta_really_associated();

    s_dns_run = false;
    if (s_dns_task) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        s_dns_task = NULL;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_provisioning = false;

    if (!s_inited) {
        s_ap_stopping = false;
        notify();
        return ESP_OK;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK && mode != WIFI_MODE_STA) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        s_ap_stopping = false;
        s_view.state = ARCHERY_WIFI_FAILED;
        set_message("关闭热点失败");
        notify();
        return err;
    }

    if (was_associated) {
        s_sta_resuming = true;
        s_resume_cancel = false;
        s_view.state = ARCHERY_WIFI_CONNECTING;
        set_message("正在恢复连接…");
        if (xTaskCreate(sta_resume_task, "wifi_resume", 4096, NULL, 3, NULL) != pdPASS) {
            s_sta_resuming = false;
            (void)esp_wifi_connect();
        }
    } else {
        s_view.state = ARCHERY_WIFI_IDLE;
        set_message("已断开");
    }
    s_ap_stopping = false;
    notify();
    ESP_LOGI(TAG, "配网热点已关闭");
    return ESP_OK;
}

void archery_wifi_set_history(const archery_history_t *history) {
    s_history = history;
}

// ---------------------------------------------------------------- 初始化
esp_err_t archery_wifi_init(archery_wifi_event_cb_t cb, void *user) {
    if (s_inited) return ESP_OK;
    s_cb = cb;
    s_cb_user = user;
    archery_wifi_view_clear(&s_view);
    s_view.state = ARCHERY_WIFI_IDLE;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_inited = true;

    char ssid[33] = {0};
    char pass[65] = {0};
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        copy_ssid(s_sta_ssid, ssid);
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_connect();
        ESP_LOGI(TAG, "使用保存的凭据连接 %s", ssid);
    }
    return ESP_OK;
}

void archery_wifi_fill_view(archery_wifi_view_t *view) {
    if (!view) return;
    *view = s_view;
    view->sta_connected = s_connected;
    view->sta_rssi = -127;
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) view->sta_rssi = ap.rssi;
    }
    strncpy(view->ip, s_ip, sizeof(view->ip) - 1);
    view->ip[sizeof(view->ip) - 1] = '\0';
}
