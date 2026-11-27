// main/archery_story.h —— 挑战意境文字。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHERY_STORY_NORMAL_COUNT 72u

const char *archery_story_normal(uint8_t index);
const char *archery_story_normal_title(uint8_t index);
const char *archery_story_hidden(uint8_t main_id, uint8_t set);

#ifdef __cplusplus
}
#endif
