// main/archery_chapters.c —— 章节解锁进度（NVS 持久化）。
#include "archery_chapters.h"

#include "nvs.h"

static const char *NVS_NAMESPACE = "chapters";
static const char *NVS_KEY = "chapter_mask";
static uint8_t s_mask;

void archery_chapters_init(void) {
    s_mask = 0u;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t stored = 0u;
    if (nvs_get_u8(handle, NVS_KEY, &stored) == ESP_OK) {
        s_mask = (uint8_t)(stored & ARCHERY_CHAPTER_ALL_MASK);
    }
    nvs_close(handle);
}

uint8_t archery_chapters_mask(void) {
    return s_mask & ARCHERY_CHAPTER_ALL_MASK;
}

bool archery_chapters_available(uint8_t set) {
    if (set == 0u) return true;
    if (set < ARCHERY_CHAPTER_STORY_COUNT) {
        return (s_mask & (uint8_t)(1u << (set - 1u))) != 0u;
    }
    if (set == ARCHERY_CHAPTER_STORY_COUNT) {
        return (s_mask & ARCHERY_CHAPTER_ALL_MASK) == ARCHERY_CHAPTER_ALL_MASK;
    }
    return false;
}

void archery_chapters_complete(uint8_t set) {
    if (set >= ARCHERY_CHAPTER_STORY_COUNT) return;
    const uint8_t next = (uint8_t)(s_mask | (uint8_t)(1u << set));
    if (next == s_mask) return;
    s_mask = next;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, NVS_KEY, s_mask) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

void archery_chapters_reset(void) {
    s_mask = 0u;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_erase_key(handle, NVS_KEY);
    (void)nvs_commit(handle);
    nvs_close(handle);
}
