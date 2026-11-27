// main/archery_challenge_voice.h —— 随机挑战本地语音播报。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 异步播放：主挑战 + 附加条件 + 难度。重复调用会先停止上一段。
void archery_challenge_voice_play(uint8_t main_id, const uint8_t *mod_ids,
                                  uint8_t mod_count, uint8_t difficulty,
                                  uint8_t volume_percent);
void archery_challenge_voice_play_cue(void);
void archery_challenge_voice_stop_cue(void);
void archery_challenge_voice_stop(void);

#ifdef __cplusplus
}
#endif
