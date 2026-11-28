// main/archery_chapters.h —— 章节解锁进度（NVS 持久化）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHERY_CHAPTER_STORY_COUNT 5u
#define ARCHERY_CHAPTER_ALL_MASK 0x1Fu

void archery_chapters_init(void);
uint8_t archery_chapters_mask(void);
bool archery_chapters_available(uint8_t set);
void archery_chapters_complete(uint8_t set);

#ifdef __cplusplus
}
#endif
