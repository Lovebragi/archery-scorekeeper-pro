// main/archery_rogue.c —— 肉鸽过滤配置的 NVS 存取。
#include "archery_rogue.h"

#include "nvs.h"

static const char *NVS_NAMESPACE = "rogue";
static const char *KEY_MAIN = "block_main";
static const char *KEY_MOD = "block_mod";
static const char *KEY_SHOW = "show_story";

static const uint8_t s_normal_ids[ARCHERY_ROGUE_MAIN_COUNT] = {
    0u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u,
    16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 33u,
};

void archery_rogue_init(void) {
}

uint32_t archery_rogue_block_main(void) {
    uint32_t value = 0u;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0u;
    (void)nvs_get_u32(handle, KEY_MAIN, &value);
    nvs_close(handle);
    return value;
}

uint8_t archery_rogue_block_mod(void) {
    uint8_t value = 0u;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0u;
    (void)nvs_get_u8(handle, KEY_MOD, &value);
    nvs_close(handle);
    return value;
}

bool archery_rogue_show_story(void) {
    uint8_t value = 1u;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return true;
    (void)nvs_get_u8(handle, KEY_SHOW, &value);
    nvs_close(handle);
    return value != 0u;
}

void archery_rogue_save(uint32_t block_main, uint8_t block_mod, bool show_story) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u32(handle, KEY_MAIN, block_main) == ESP_OK &&
        nvs_set_u8(handle, KEY_MOD, block_mod) == ESP_OK &&
        nvs_set_u8(handle, KEY_SHOW, show_story ? 1u : 0u) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

uint8_t archery_rogue_main_id(uint8_t index) {
    return index < ARCHERY_ROGUE_MAIN_COUNT ? s_normal_ids[index] : 0xFFu;
}

uint8_t archery_rogue_main_index(uint8_t main_id) {
    for (uint8_t i = 0; i < ARCHERY_ROGUE_MAIN_COUNT; i++) {
        if (s_normal_ids[i] == main_id) return i;
    }
    return 0xFFu;
}
