// main/archery_voice.c —— 实验：PTT 报靶语音上传与结果解析。
#include "archery_voice.h"
#include "esp_timer.h"
#include "archery_countdown_pcm.h"
#include "archery_countdown_system_pcm.h"
#include "archery_countdown_bao_pcm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "archery_score.h"
#include "archery_speech.h"
#include "archery_ws_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "archery_voice";

#define VOICE_NVS_NAMESPACE "archery_net"
#define VOICE_NVS_KEY "voice_url"
#define VOICE_URL_MAX 128
#define VOICE_CHUNK_BYTES 640  // 20 ms @ 16 kHz / 16 bit
#define VOICE_MAX_MS 8000
#define VOICE_RESPONSE_MAX 1200

typedef struct {
    char host[64];
    uint16_t port;
    char path[64];
} voice_endpoint_t;

static char s_url[VOICE_URL_MAX];
static char s_status[96] = "长按上键说话";
static char s_target_name[8] = "half";
static uint8_t s_target = 0;
static uint8_t s_arrows = 6;
static char s_appid[32];
static char s_api_key[192];
static char s_token[96];
static uint8_t s_beep_volume = 60;
static bool s_audio_ready;
static uint8_t s_countdown_mode = 3;
static voice_endpoint_t s_endpoint;
static SemaphoreHandle_t s_start_sem;
static TaskHandle_t s_task;
static volatile bool s_active;
static volatile bool s_stop_requested;
static volatile bool s_result_ready;
static esp_timer_handle_t s_stop_timer;
static archery_voice_result_t s_result;

static bool parse_url(const char *url, voice_endpoint_t *out) {
    if (!url || !out) return false;
    if (strncmp(url, "http://", 7) != 0) return false;
    const char *rest = url + 7;
    const char *slash = strchr(rest, '/');
    if (!slash) return false;
    const char *colon = memchr(rest, ':', (size_t)(slash - rest));
    size_t host_len = colon ? (size_t)(colon - rest) : (size_t)(slash - rest);
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, rest, host_len);
    out->host[host_len] = '\0';
    out->port = 80;
    if (colon) {
        const long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) return false;
        out->port = (uint16_t)port;
    }
    if (strlen(slash) >= sizeof(out->path)) return false;
    strcpy(out->path, slash);
    return true;
}

static esp_err_t load_url(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOICE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t length = sizeof(s_url);
    s_url[0] = '\0';
    err = nvs_get_str(handle, VOICE_NVS_KEY, s_url, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_url[0] = '\0';
        return ESP_OK;
    }
    return err;
}

esp_err_t archery_voice_save_url(const char *url) {
    if (!url) return ESP_ERR_INVALID_ARG;
    voice_endpoint_t probe;
    if (url[0] && !parse_url(url, &probe)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOICE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, VOICE_NVS_KEY, url);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        strncpy(s_url, url, sizeof(s_url) - 1);
        s_url[sizeof(s_url) - 1] = '\0';
        archery_voice_set_status(s_url[0] ? "长按上键说话" : "语音未配置");
    }
    return err;
}

bool archery_voice_enabled(void) { return s_url[0] != '\0' || s_api_key[0] || (s_appid[0] && s_token[0]); }
const char *archery_voice_url(void) { return s_url; }
bool archery_voice_active(void) { return s_active; }
const char *archery_voice_status(void) { return s_status; }

void archery_voice_set_status(const char *text) {
    if (!text) {
        s_status[0] = '\0';
        return;
    }
    strncpy(s_status, text, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

void archery_voice_set_context(uint8_t target, uint8_t arrows) {
    s_target = target;
    s_arrows = arrows ? arrows : 6;
    switch (target) {
    case ARCHERY_TARGET_FULL_40:
    case ARCHERY_TARGET_FULL_80:
    case ARCHERY_TARGET_FULL_122:
        strcpy(s_target_name, "full");
        break;
    default:
        strcpy(s_target_name, "half");
        break;
    }
}

uint8_t archery_voice_context_target(void) { return s_target; }
uint8_t archery_voice_context_arrows(void) { return s_arrows; }
const char *archery_voice_appid(void) { return s_appid; }
const char *archery_voice_api_key(void) { return s_api_key; }

bool archery_voice_test_credentials(void) {
    if (!archery_voice_has_credentials()) return false;
    if (s_active) return false;
    const bool ok = archery_ws_open(s_appid, s_token, s_api_key);
    if (ok) archery_ws_close();
    return ok;
}

esp_err_t archery_voice_save_api_key(const char *api_key) {
    if (!api_key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOICE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "api_key", api_key);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) {
        (void)nvs_erase_key(handle, "appid");
        (void)nvs_erase_key(handle, "token");
        (void)nvs_erase_key(handle, VOICE_NVS_KEY);
        (void)nvs_commit(handle);
        s_appid[0] = '\0';
        s_token[0] = '\0';
        s_url[0] = '\0';
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
        s_api_key[sizeof(s_api_key) - 1] = '\0';
    }
    return err;
}
const char *archery_voice_token(void) { return s_token; }
bool archery_voice_has_credentials(void) { return s_api_key[0] || (s_appid[0] && s_token[0]); }

esp_err_t archery_voice_save_credentials(const char *appid, const char *token) {
    if (!appid || !token) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOICE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "appid", appid);
    if (err == ESP_OK) err = nvs_set_str(handle, "token", token);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        strncpy(s_appid, appid, sizeof(s_appid) - 1);
        strncpy(s_token, token, sizeof(s_token) - 1);
        s_appid[sizeof(s_appid) - 1] = '\0';
        s_token[sizeof(s_token) - 1] = '\0';
        archery_voice_set_status("凭据已保存");
    }
    return err;
}

static void load_credentials(void) {
    nvs_handle_t handle;
    if (nvs_open(VOICE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(s_appid);
    if (nvs_get_str(handle, "appid", s_appid, &length) != ESP_OK) s_appid[0] = '\0';
    length = sizeof(s_token);
    if (nvs_get_str(handle, "token", s_token, &length) != ESP_OK) s_token[0] = '\0';
    length = sizeof(s_api_key);
    if (nvs_get_str(handle, "api_key", s_api_key, &length) != ESP_OK) s_api_key[0] = '\0';
    nvs_close(handle);
}

bool archery_voice_take_result(archery_voice_result_t *out) {
    if (!s_result_ready || !out) return false;
    *out = s_result;
    s_result_ready = false;
    return true;
}

static int connect_to(const voice_endpoint_t *ep) {
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ep->port);
    if (inet_pton(AF_INET, ep->host, &dest.sin_addr) != 1) {
        struct hostent *he = gethostbyname(ep->host);
        if (!he || !he->h_addr_list[0]) return -1;
        memcpy(&dest.sin_addr, he->h_addr_list[0], sizeof(dest.sin_addr));
    }
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static bool send_all(int sock, const void *data, size_t length) {
    const uint8_t *ptr = (const uint8_t *)data;
    while (length > 0) {
        const int sent = send(sock, ptr, length, 0);
        if (sent <= 0) return false;
        ptr += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool send_chunk(int sock, const void *data, size_t length) {
    char header[12];
    const int header_len = snprintf(header, sizeof(header), "%x\r\n", (unsigned)length);
    return send_all(sock, header, (size_t)header_len) &&
           send_all(sock, data, length) &&
           send_all(sock, "\r\n", 2);
}

static size_t read_response(int sock, char *buffer, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        const int got = recv(sock, buffer + used, capacity - 1 - used, 0);
        if (got <= 0) break;
        used += (size_t)got;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n") && strstr(buffer, "}")) break;
    }
    buffer[used] = '\0';
    return used;
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static void parse_result(const char *json) {
    memset(&s_result, 0, sizeof(s_result));
    const char *ok = json ? strstr(json, "\"ok\"") : NULL;
    const char *ok_colon = ok ? strchr(ok, ':') : NULL;
    if (!ok_colon || strncmp(skip_spaces(ok_colon + 1), "true", 4) != 0) {
        strncpy(s_result.error, "parse", sizeof(s_result.error) - 1);
        return;
    }
    s_result.ok = true;
    const char *cursor = json;
    while (s_result.count < ARCHERY_VOICE_MAX_ITEMS) {
        const char *ring = strstr(cursor, "\"ring\"");
        if (!ring) break;
        const char *ring_colon = strchr(ring, ':');
        if (!ring_colon) break;
        int ring_value = -1;
        if (sscanf(skip_spaces(ring_colon + 1), "%d", &ring_value) != 1) break;
        const char *count = strstr(ring, "\"count\"");
        const char *next = strstr(ring + 6, "\"ring\"");
        if (!count || (next && count > next)) break;
        const char *count_colon = strchr(count, ':');
        if (!count_colon) break;
        const char *count_value = skip_spaces(count_colon + 1);
        archery_voice_item_t *item = &s_result.items[s_result.count];
        item->ring = (uint8_t)ring_value;
        if (strncmp(count_value, "null", 4) == 0) {
            item->all = true;
        } else {
            int parsed_count = -1;
            if (sscanf(count_value, "%d", &parsed_count) != 1) break;
            item->count = (uint8_t)parsed_count;
        }
        s_result.count++;
        cursor = next ? next : count_colon + 1;
    }
    if (s_result.count == 0) s_result.ok = false;
}

static void play_tone(uint16_t hz, uint16_t ms, int repeat, uint16_t gap_ms) {
    if (s_beep_volume == 0) return;  // 静音档：不碰 codec
    bsp_audio_set_volume(s_beep_volume);
    int16_t buffer[320];
    const int samples = (int)(16000 / hz / 2);
    for (int n = 0; n < repeat; n++) {
        int phase = 0;
        int remaining = (int)(16000L * ms / 1000);
        while (remaining > 0) {
            const int chunk = remaining < (int)(sizeof(buffer) / sizeof(buffer[0]))
                                  ? remaining : (int)(sizeof(buffer) / sizeof(buffer[0]));
            for (int i = 0; i < chunk; i++) {
                buffer[i] = phase < samples ? 5000 : -5000;
                if (++phase >= samples * 2) phase = 0;
            }
            (void)bsp_audio_write(buffer, (size_t)chunk * sizeof(buffer[0]));
            remaining -= chunk;
        }
        if (n + 1 < repeat && gap_ms) vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }
}

// 直连豆包流式识别：录完一段后解析成环值条目。
static const char *open_failure_status(void) {
    const int code = archery_ws_last_status();
    return (code == 401 || code == 403) ? "语音凭据无效，请重设" : "连不上豆包";
}

static void run_ws_session(void) {
    if (!archery_ws_connected()) {
        if (!archery_ws_open(s_appid, s_token, s_api_key)) {
            archery_voice_set_status(open_failure_status());
            play_tone(600, 150, 2, 100);
            s_result_ready = true;
            return;
        }
    }
    archery_voice_set_status("录音中，松手结束");
    play_tone(1000, 80, 1, 0);

    // 预热连接可能已被服务端关闭：先发 10 ms 静音探活，失败就重连一次。
    int16_t probe[160] = {0};
    if (!archery_ws_send_audio(probe, 160, false)) {
        if (s_stop_timer) (void)esp_timer_stop(s_stop_timer);
    archery_ws_close();
        if (!archery_ws_open(s_appid, s_token, s_api_key)) {
            archery_voice_set_status(open_failure_status());
            play_tone(600, 150, 2, 100);
            s_result_ready = true;
            return;
        }
    }

    uint8_t pcm[640];
    const int64_t started = esp_timer_get_time();
    while (!s_stop_requested && (esp_timer_get_time() - started) / 1000 < VOICE_MAX_MS) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) break;
        if (!archery_ws_send_audio((const int16_t *)pcm, sizeof(pcm) / 2, false)) break;
    }
    int16_t tail[160] = {0};
    (void)archery_ws_send_audio(tail, 160, true);

    char text[256] = {0};
    char latest[256] = {0};
    const int64_t wait_started = esp_timer_get_time();
    for (int i = 0; i < 300 && (esp_timer_get_time() - wait_started) / 1000 < 1000; i++) {
        const bool last = archery_ws_read(text, sizeof(text));
        if (text[0]) {
            strncpy(latest, text, sizeof(latest) - 1);
            latest[sizeof(latest) - 1] = '\0';
        }
        if (last) break;
        if (!archery_ws_connected()) break;
    }
    if (s_stop_timer) (void)esp_timer_stop(s_stop_timer);
    archery_ws_close();

    archery_speech_result_t parsed;
    archery_speech_parse(latest, s_target, s_arrows, &parsed);
    const int ws_err = archery_ws_last_error_code();
    if (ws_err && ws_err != 45000081) {
        ESP_LOGI(TAG, "asr=%s target=%u ok=%d ws_err=%d %s", latest, (unsigned)s_target,
                 (int)parsed.ok, ws_err, archery_ws_last_error_msg());
    } else {
        ESP_LOGI(TAG, "asr=%s target=%u ok=%d", latest, (unsigned)s_target, (int)parsed.ok);
    }
    memset(&s_result, 0, sizeof(s_result));
    if (parsed.ok) {
        s_result.ok = true;
        s_result.count = parsed.count;
        for (uint8_t i = 0; i < parsed.count; i++) {
            s_result.items[i].ring = parsed.items[i].ring;
            s_result.items[i].count = parsed.items[i].count;
            s_result.items[i].all = parsed.items[i].all;
        }
        archery_voice_set_status("识别完成");
        play_tone(1000, 80, 1, 0);
    } else {
        strncpy(s_result.error, "parse", sizeof(s_result.error) - 1);
        char msg[128];
        if (latest[0]) {
            snprintf(msg, sizeof(msg), "%s：%.60s",
                     parsed.error == ARCHERY_SPEECH_MISMATCH ? "数量对不上" : "没听清",
                     latest);
        } else {
            snprintf(msg, sizeof(msg), "没听清：未识别到文字");
        }
        archery_voice_set_status(msg);
        play_tone(600, 150, 2, 100);
    }
    s_result_ready = true;
}

static void run_session(void) {
    s_active = true;
    s_stop_requested = false;
    s_result_ready = false;
    archery_voice_set_status("录音中，松手结束");

    if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        archery_voice_set_status("麦克风初始化失败");
        play_tone(600, 150, 2, 100);
        s_active = false;
        return;
    }
    s_audio_ready = true;

    if (archery_voice_has_credentials()) {
        run_ws_session();
        s_active = false;
        return;
    }

    const int sock = connect_to(&s_endpoint);
    if (sock < 0) {
        archery_voice_set_status("连不上语音服务");
        play_tone(600, 150, 2, 100);
        s_active = false;
        return;
    }

    char request[256];
    const int request_len = snprintf(
        request, sizeof(request),
        "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
        "Content-Type: audio/pcm;rate=16000;bits=16;channels=1\r\n"
        "X-Archery-Target: %s\r\nX-Archery-Arrows: %u\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
        s_endpoint.path, s_endpoint.host, (unsigned)s_endpoint.port,
        s_target_name, (unsigned)s_arrows);
    if (!send_all(sock, request, (size_t)request_len)) {
        close(sock);
        archery_voice_set_status("发送失败");
        play_tone(600, 150, 2, 100);
        s_active = false;
        return;
    }

    uint8_t pcm[VOICE_CHUNK_BYTES];
    const int64_t started = esp_timer_get_time();
    bool sent_ok = true;
    while (!s_stop_requested && (esp_timer_get_time() - started) / 1000 < VOICE_MAX_MS) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            sent_ok = false;
            break;
        }
        if (!send_chunk(sock, pcm, sizeof(pcm))) {
            sent_ok = false;
            break;
        }
    }
    send_all(sock, "0\r\n\r\n", 5);

    char response[VOICE_RESPONSE_MAX];
    const size_t got = read_response(sock, response, sizeof(response));
    close(sock);

    const char *body = strstr(response, "\r\n\r\n");
    if (sent_ok && body) {
        parse_result(body + 4);
    }
    if (!s_result.ok) {
        if (!s_result.error[0]) {
            strncpy(s_result.error, "network", sizeof(s_result.error) - 1);
        }
        archery_voice_set_status("没听清，请重说");
        play_tone(600, 150, 2, 100);
    } else {
        play_tone(1000, 80, 1, 0);
    }
    s_result_ready = true;
    s_active = false;
    if (s_result.ok) archery_voice_set_status("识别完成");
    ESP_LOGI(TAG, "session done: ok=%d items=%u bytes=%u", (int)s_result.ok,
             (unsigned)s_result.count, (unsigned)got);
}

static void voice_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_start_sem, portMAX_DELAY) == pdTRUE) {
            run_session();
        }
    }
}

esp_err_t archery_voice_init(void) {
    esp_err_t err = load_url();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "语音地址读取失败: %s", esp_err_to_name(err));
    }
    load_credentials();
    s_start_sem = xSemaphoreCreateBinary();
    if (!s_start_sem) return ESP_ERR_NO_MEM;
    if (xTaskCreate(voice_task, "voice", 16384, NULL, 4, &s_task) != pdPASS) {
        vSemaphoreDelete(s_start_sem);
        s_start_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    archery_voice_set_status((s_appid[0] && s_token[0]) || s_url[0]
                                 ? "长按上键说话"
                                 : "语音未配置");
    ESP_LOGI(TAG, "voice ready, url=%s creds=%d", s_url[0] ? s_url : "(none)",
             (s_appid[0] && s_token[0]) ? 1 : 0);
    return ESP_OK;
}

static void prewarm_task(void *arg) {
    (void)arg;
    if (!archery_ws_connected() && archery_voice_has_credentials() && !s_active) {
        if (archery_ws_open(s_appid, s_token, s_api_key)) {
            archery_voice_set_status("语音就绪");
        }
    }
    vTaskDelete(NULL);
}

void archery_voice_set_countdown_voice(uint8_t mode) { s_countdown_mode = mode > 3 ? 3 : mode; }

void archery_voice_set_beep_volume(uint8_t percent) {
    s_beep_volume = percent > 100 ? 100 : percent;
}

static const int16_t *countdown_clip(uint8_t number, unsigned *samples) {
    switch (number) {
    case 1: *samples = countdown_1_samples; return countdown_1;
    case 2: *samples = countdown_2_samples; return countdown_2;
    case 3: *samples = countdown_3_samples; return countdown_3;
    case 4: *samples = countdown_4_samples; return countdown_4;
    case 5: *samples = countdown_5_samples; return countdown_5;
    case 6: *samples = countdown_6_samples; return countdown_6;
    case 7: *samples = countdown_7_samples; return countdown_7;
    case 8: *samples = countdown_8_samples; return countdown_8;
    case 9: *samples = countdown_9_samples; return countdown_9;
    case 10: *samples = countdown_10_samples; return countdown_10;
    default: *samples = 0; return NULL;
    }
}

static const int16_t *system_clip(uint8_t number, unsigned *samples) {
    switch (number) {
    case 1: *samples = countdown_sys_1_samples; return countdown_sys_1;
    case 2: *samples = countdown_sys_2_samples; return countdown_sys_2;
    case 3: *samples = countdown_sys_3_samples; return countdown_sys_3;
    case 4: *samples = countdown_sys_4_samples; return countdown_sys_4;
    case 5: *samples = countdown_sys_5_samples; return countdown_sys_5;
    case 6: *samples = countdown_sys_6_samples; return countdown_sys_6;
    case 7: *samples = countdown_sys_7_samples; return countdown_sys_7;
    case 8: *samples = countdown_sys_8_samples; return countdown_sys_8;
    case 9: *samples = countdown_sys_9_samples; return countdown_sys_9;
    case 10: *samples = countdown_sys_10_samples; return countdown_sys_10;
    default: *samples = 0; return NULL;
    }
}

static const int16_t *bao_clip(uint8_t number, unsigned *samples) {
    switch (number) {
    case 1: *samples = countdown_bao_1_samples; return countdown_bao_1;
    case 2: *samples = countdown_bao_2_samples; return countdown_bao_2;
    case 3: *samples = countdown_bao_3_samples; return countdown_bao_3;
    case 4: *samples = countdown_bao_4_samples; return countdown_bao_4;
    case 5: *samples = countdown_bao_5_samples; return countdown_bao_5;
    case 6: *samples = countdown_bao_6_samples; return countdown_bao_6;
    case 7: *samples = countdown_bao_7_samples; return countdown_bao_7;
    case 8: *samples = countdown_bao_8_samples; return countdown_bao_8;
    case 9: *samples = countdown_bao_9_samples; return countdown_bao_9;
    case 10: *samples = countdown_bao_10_samples; return countdown_bao_10;
    default: *samples = 0; return NULL;
    }
}

void archery_voice_play_countdown(uint8_t number) {
    if (s_active || s_beep_volume == 0) return;
    unsigned samples = 0;
    if (s_countdown_mode == 0) return;
    const int16_t *clip = NULL;
    if (s_countdown_mode == 1) clip = system_clip(number, &samples);
    else if (s_countdown_mode == 2) clip = countdown_clip(number, &samples);
    else clip = bao_clip(number, &samples);
    if (!clip || samples == 0) return;
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            return;
        }
        s_audio_ready = true;
    }
    bsp_audio_set_volume(s_beep_volume);
    (void)bsp_audio_write(clip, (size_t)samples * sizeof(clip[0]));
}

void archery_voice_metronome_click(bool accent, uint8_t volume_percent) {
    if (volume_percent == 0) return;
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) return;
        s_audio_ready = true;
    }
    bsp_audio_set_volume(volume_percent > 100 ? 100 : volume_percent);
    const uint16_t hz = accent ? 1600 : 1100;
    const int samples = 16000 / hz / 2;
    int16_t buffer[320];
    const int amp = volume_percent > 100 ? 6000 : 5000;
    int phase = 0;
    for (int i = 0; i < 320; i++) {
        buffer[i] = phase < samples ? amp : -amp;
        if (++phase >= samples * 2) phase = 0;
    }
    (void)bsp_audio_write(buffer, sizeof(buffer));
}

void archery_voice_beep_warning(void) {
    if (s_active || s_beep_volume == 0) return;
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            return;
        }
        s_audio_ready = true;
    }
    // C 大调 do-mi-sol 上行琶音。
    play_tone(523, 120, 1, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    play_tone(659, 120, 1, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    play_tone(784, 160, 1, 0);
}

void archery_voice_preview_beep(void) {
    if (s_active || s_beep_volume == 0) return;
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            return;
        }
        s_audio_ready = true;
    }
    play_tone(1000, 80, 1, 0);
}

void archery_voice_reopen_audio(void) {
    s_audio_ready = false;
    if (bsp_audio_init() == ESP_OK && bsp_audio_set_format(16000, 16, 1) == ESP_OK) {
        s_audio_ready = true;
    }
}

void archery_voice_prepare_audio(void) {
    if (s_audio_ready) return;
    if (bsp_audio_init() == ESP_OK && bsp_audio_set_format(16000, 16, 1) == ESP_OK) {
        s_audio_ready = true;
    }
}

void archery_voice_play_saved(void) {
    ESP_LOGI(TAG, "play_saved vol=%u active=%d ready=%d", (unsigned)s_beep_volume, (int)s_active, (int)s_audio_ready);
    if (s_active || s_beep_volume == 0) return;
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            return;
        }
        s_audio_ready = true;
    }
    bsp_audio_set_volume(s_beep_volume);
    play_tone(523, 180, 1, 0);   // C5 do
    vTaskDelay(pdMS_TO_TICKS(300));
    play_tone(784, 220, 1, 0);   // G5 sol
}

void archery_voice_play_config_failed(void) {
    ESP_LOGI(TAG, "play_failed vol=%u active=%d ready=%d", (unsigned)s_beep_volume, (int)s_active, (int)s_audio_ready);
    if (s_active || s_beep_volume == 0) return;
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            return;
        }
        s_audio_ready = true;
    }
    bsp_audio_set_volume(s_beep_volume);
    play_tone(784, 180, 1, 0);   // G5 sol
    vTaskDelay(pdMS_TO_TICKS(300));
    play_tone(523, 220, 1, 0);   // C5 do
}
void archery_voice_prewarm(void) {
    if (!archery_voice_has_credentials() || archery_ws_connected() || s_active) return;
    xTaskCreate(prewarm_task, "voice_pre", 12288, NULL, 3, NULL);
}

esp_err_t archery_voice_start(void) {
    if (!archery_voice_enabled()) return ESP_ERR_INVALID_STATE;
    if (!archery_voice_has_credentials() && !parse_url(s_url, &s_endpoint)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_active) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_start_sem);
    return ESP_OK;
}

static void stop_timer_cb(void *arg) {
    (void)arg;
    if (s_active) archery_ws_abort();
}

void archery_voice_stop(void) {
    if (!s_active) return;
    s_stop_requested = true;
    if (!s_stop_timer) {
        const esp_timer_create_args_t args = {.callback = stop_timer_cb, .name = "asr_stop"};
        if (esp_timer_create(&args, &s_stop_timer) != ESP_OK) s_stop_timer = NULL;
    }
    if (s_stop_timer) {
        (void)esp_timer_stop(s_stop_timer);
        (void)esp_timer_start_once(s_stop_timer, 1200000);
    }
}
