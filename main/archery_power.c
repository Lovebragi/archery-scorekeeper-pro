// main/archery_power.c —— 深度睡眠时间设置（独立 NVS，避免改设置格式）。
#include "archery_power.h"

#include "nvs.h"

static const char *NVS_NS = "archery_pwr";
static const char *NVS_KEY = "deep_min";

uint8_t archery_power_deep_minutes(void) {
    nvs_handle_t h;
    uint8_t v = 5;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY, &v) != ESP_OK) v = 5;
        nvs_close(h);
    }
    if (v != 3 && v != 5 && v != 10 && v != 15) v = 5;
    return v;
}

void archery_power_cycle_deep(void) {
    uint8_t v = archery_power_deep_minutes();
    v = v == 3 ? 5 : (v == 5 ? 10 : (v == 10 ? 15 : 3));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, NVS_KEY, v);
    (void)nvs_commit(h);
    nvs_close(h);
}
