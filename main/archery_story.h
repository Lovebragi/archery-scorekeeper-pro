// main/archery_story.h —— 挑战意境文字。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHERY_STORY_SET_COUNT 6u
#define ARCHERY_STORY_LEVEL_COUNT 12u
#define ARCHERY_STORY_NORMAL_COUNT (ARCHERY_STORY_SET_COUNT * ARCHERY_STORY_LEVEL_COUNT)

const char *archery_story_normal(uint8_t index);
const char *archery_story_normal_title(uint8_t index);
const char *archery_story_hidden(uint8_t main_id, uint8_t set);
const char *archery_story_set_name(uint8_t set);
const char *archery_story_ending_title(uint8_t set);
const char *archery_story_ending_text(uint8_t set);

#ifdef __cplusplus
}
#endif
