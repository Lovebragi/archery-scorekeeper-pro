// main/archery_metro.c —— 节拍器参数的 NVS 读写。
#include "archery_metro.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

#define METRO_NAMESPACE "archery"
#define METRO_KEY "metro"
#define METRO_MAGIC 0x4Du
#define METRO_VERSION 1u

void archery_metro_defaults(archery_metro_config_t *config) {
    if (!config) return;
    config->bpm = 60;
    config->accent = 4;
    config->countin = 0;
    config->stop_min = 0;
    config->volume = 100;
    config->preset = 0;
}

bool archery_metro_load(archery_metro_config_t *config) {
    if (!config) return false;
    archery_metro_defaults(config);
#ifndef ESP_PLATFORM
    return false;
#else
    nvs_handle_t handle;
    if (nvs_open(METRO_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t buffer[10];
    size_t length = sizeof(buffer);
    const esp_err_t err = nvs_get_blob(handle, METRO_KEY, buffer, &length);
    nvs_close(handle);
    if (err != ESP_OK || length != sizeof(buffer) || buffer[0] != METRO_MAGIC ||
        buffer[1] != METRO_VERSION) {
        return false;
    }
    config->bpm = (uint16_t)(buffer[2] | ((uint16_t)buffer[3] << 8));
    config->accent = buffer[4];
    config->countin = buffer[5];
    config->stop_min = buffer[6];
    config->volume = buffer[7];
    config->preset = buffer[8];
    if (config->bpm < 30 || config->bpm > 240) config->bpm = 60;
    if (config->accent < 1 || config->accent > 16) config->accent = 4;
    if (config->countin > 8) config->countin = 0;
    if (config->volume < 20 || config->volume > 120) config->volume = 100;
    return true;
#endif
}

bool archery_metro_save(const archery_metro_config_t *config) {
    if (!config) return false;
#ifndef ESP_PLATFORM
    return false;
#else
    uint8_t buffer[10] = {0};
    buffer[0] = METRO_MAGIC;
    buffer[1] = METRO_VERSION;
    buffer[2] = (uint8_t)(config->bpm & 0xFFu);
    buffer[3] = (uint8_t)(config->bpm >> 8);
    buffer[4] = config->accent;
    buffer[5] = config->countin;
    buffer[6] = config->stop_min;
    buffer[7] = config->volume;
    buffer[8] = config->preset;
    nvs_handle_t handle;
    if (nvs_open(METRO_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, METRO_KEY, buffer, sizeof(buffer));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
#endif
}
