// main/archery_challenge_voice.c —— 挑战提示音的本地 PCM 拼接播放。
#include "archery_challenge_voice.h"

#include "archery_challenge.h"
#include "bsp_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"


#define CLIP_SYMBOL(name) \
    extern const uint8_t _binary_##name##_start[]; \
    extern const uint8_t _binary_##name##_end[]

CLIP_SYMBOL(hidden_cue_pcm);
CLIP_SYMBOL(hidden_intro_pcm);
CLIP_SYMBOL(main_00_pcm);
CLIP_SYMBOL(main_01_pcm);
CLIP_SYMBOL(main_02_pcm);
CLIP_SYMBOL(main_03_pcm);
CLIP_SYMBOL(main_04_pcm);
CLIP_SYMBOL(main_05_pcm);
CLIP_SYMBOL(main_06_pcm);
CLIP_SYMBOL(main_07_pcm);
CLIP_SYMBOL(main_08_pcm);
CLIP_SYMBOL(main_09_pcm);
CLIP_SYMBOL(main_10_pcm);
CLIP_SYMBOL(main_11_pcm);
CLIP_SYMBOL(main_12_pcm);
CLIP_SYMBOL(main_13_pcm);
CLIP_SYMBOL(main_14_pcm);
CLIP_SYMBOL(main_15_pcm);
CLIP_SYMBOL(main_16_pcm);
CLIP_SYMBOL(main_17_pcm);
CLIP_SYMBOL(main_18_pcm);
CLIP_SYMBOL(main_19_pcm);
CLIP_SYMBOL(main_20_pcm);
CLIP_SYMBOL(main_21_pcm);
CLIP_SYMBOL(main_22_pcm);
CLIP_SYMBOL(main_23_pcm);
CLIP_SYMBOL(main_24_pcm);
CLIP_SYMBOL(main_25_pcm);
CLIP_SYMBOL(main_26_pcm);
CLIP_SYMBOL(main_27_pcm);
CLIP_SYMBOL(main_28_pcm);
CLIP_SYMBOL(main_29_pcm);
CLIP_SYMBOL(main_32_pcm);
CLIP_SYMBOL(main_33_pcm);
CLIP_SYMBOL(main_31_pcm);
CLIP_SYMBOL(main_30_pcm);

CLIP_SYMBOL(mod_01_pcm);
CLIP_SYMBOL(mod_02_pcm);
CLIP_SYMBOL(mod_03_pcm);
CLIP_SYMBOL(mod_04_pcm);
CLIP_SYMBOL(mod_05_pcm);
CLIP_SYMBOL(mod_08_pcm);
CLIP_SYMBOL(mod_10_pcm);
CLIP_SYMBOL(mod_11_pcm);

CLIP_SYMBOL(diff_01_pcm);
CLIP_SYMBOL(diff_02_pcm);
CLIP_SYMBOL(diff_03_pcm);
CLIP_SYMBOL(diff_04_pcm);
CLIP_SYMBOL(diff_05_pcm);
CLIP_SYMBOL(diff_06_pcm);
CLIP_SYMBOL(diff_07_pcm);
CLIP_SYMBOL(diff_08_pcm);
CLIP_SYMBOL(diff_09_pcm);
CLIP_SYMBOL(diff_10_pcm);
CLIP_SYMBOL(diff_11_pcm);
CLIP_SYMBOL(diff_12_pcm);
CLIP_SYMBOL(diff_15_pcm);
CLIP_SYMBOL(diff_14_pcm);
CLIP_SYMBOL(diff_13_pcm);

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} clip_t;

static const clip_t s_cue = {_binary_hidden_cue_pcm_start, _binary_hidden_cue_pcm_end};
static const clip_t s_intro = {_binary_hidden_intro_pcm_start, _binary_hidden_intro_pcm_end};

static const clip_t s_main[] = {
    {_binary_main_00_pcm_start, _binary_main_00_pcm_end},
    {_binary_main_01_pcm_start, _binary_main_01_pcm_end},
    {_binary_main_02_pcm_start, _binary_main_02_pcm_end},
    {_binary_main_03_pcm_start, _binary_main_03_pcm_end},
    {_binary_main_04_pcm_start, _binary_main_04_pcm_end},
    {_binary_main_05_pcm_start, _binary_main_05_pcm_end},
    {_binary_main_06_pcm_start, _binary_main_06_pcm_end},
    {_binary_main_07_pcm_start, _binary_main_07_pcm_end},
    {_binary_main_08_pcm_start, _binary_main_08_pcm_end},
    {_binary_main_09_pcm_start, _binary_main_09_pcm_end},
    {_binary_main_10_pcm_start, _binary_main_10_pcm_end},
    {_binary_main_11_pcm_start, _binary_main_11_pcm_end},
    {_binary_main_12_pcm_start, _binary_main_12_pcm_end},
    {_binary_main_13_pcm_start, _binary_main_13_pcm_end},
    {_binary_main_14_pcm_start, _binary_main_14_pcm_end},
    {_binary_main_15_pcm_start, _binary_main_15_pcm_end},
    {_binary_main_16_pcm_start, _binary_main_16_pcm_end},
    {_binary_main_17_pcm_start, _binary_main_17_pcm_end},
    {_binary_main_18_pcm_start, _binary_main_18_pcm_end},
    {_binary_main_19_pcm_start, _binary_main_19_pcm_end},
    {_binary_main_20_pcm_start, _binary_main_20_pcm_end},
    {_binary_main_21_pcm_start, _binary_main_21_pcm_end},
    {_binary_main_22_pcm_start, _binary_main_22_pcm_end},
    {_binary_main_23_pcm_start, _binary_main_23_pcm_end},
    {_binary_main_24_pcm_start, _binary_main_24_pcm_end},
    {_binary_main_25_pcm_start, _binary_main_25_pcm_end},
    {_binary_main_26_pcm_start, _binary_main_26_pcm_end},
    {_binary_main_27_pcm_start, _binary_main_27_pcm_end},
    {_binary_main_28_pcm_start, _binary_main_28_pcm_end},
    {_binary_main_29_pcm_start, _binary_main_29_pcm_end},
    {_binary_main_30_pcm_start, _binary_main_30_pcm_end},
    {_binary_main_31_pcm_start, _binary_main_31_pcm_end},
    {_binary_main_32_pcm_start, _binary_main_32_pcm_end},
    {_binary_main_33_pcm_start, _binary_main_33_pcm_end},
};
static const clip_t s_mod[] = {
    {_binary_mod_01_pcm_start, _binary_mod_01_pcm_end},  // T4
    {_binary_mod_08_pcm_start, _binary_mod_08_pcm_end},  // T5
    {_binary_mod_02_pcm_start, _binary_mod_02_pcm_end},  // P1
    {_binary_mod_03_pcm_start, _binary_mod_03_pcm_end},  // P3
    {_binary_mod_04_pcm_start, _binary_mod_04_pcm_end},  // P4
    {_binary_mod_10_pcm_start, _binary_mod_10_pcm_end},  // P5
    {_binary_mod_11_pcm_start, _binary_mod_11_pcm_end},  // P6
    {_binary_mod_05_pcm_start, _binary_mod_05_pcm_end},  // P8
};
static const clip_t s_diff[] = {
    {_binary_diff_01_pcm_start, _binary_diff_01_pcm_end},
    {_binary_diff_02_pcm_start, _binary_diff_02_pcm_end},
    {_binary_diff_03_pcm_start, _binary_diff_03_pcm_end},
    {_binary_diff_04_pcm_start, _binary_diff_04_pcm_end},
    {_binary_diff_05_pcm_start, _binary_diff_05_pcm_end},
    {_binary_diff_06_pcm_start, _binary_diff_06_pcm_end},
    {_binary_diff_07_pcm_start, _binary_diff_07_pcm_end},
    {_binary_diff_08_pcm_start, _binary_diff_08_pcm_end},
    {_binary_diff_09_pcm_start, _binary_diff_09_pcm_end},
    {_binary_diff_10_pcm_start, _binary_diff_10_pcm_end},
    {_binary_diff_11_pcm_start, _binary_diff_11_pcm_end},
    {_binary_diff_12_pcm_start, _binary_diff_12_pcm_end},
    {_binary_diff_13_pcm_start, _binary_diff_13_pcm_end},
    {_binary_diff_14_pcm_start, _binary_diff_14_pcm_end},
    {_binary_diff_15_pcm_start, _binary_diff_15_pcm_end},
};
static TaskHandle_t s_task;
static TaskHandle_t s_cue_task;
static volatile bool s_stop;
static volatile bool s_cue_stop;
static uint8_t s_main_id;
static uint8_t s_mod_ids[ARCHERY_CHALLENGE_MAX_MODS];
static uint8_t s_mod_count;
static uint8_t s_difficulty;
static uint8_t s_volume = 60;
static uint32_t s_hz;

static bool write_all(const clip_t *clip, uint32_t hz) {
    if (!clip || !clip->start || !clip->end || clip->end <= clip->start) return true;
    if (hz != s_hz) {
        if (bsp_audio_set_format(hz, 16, 1) != ESP_OK) return false;
        s_hz = hz;
    }
    const uint8_t *p = clip->start;
    while (p < clip->end) {
        if (s_stop) return false;
        size_t chunk = (size_t)(clip->end - p);
        if (chunk > 2048u) chunk = 2048u;
        if (bsp_audio_write(p, chunk) != ESP_OK) return false;
        p += chunk;
    }
    return true;
}

static void write_silence(void) {
    static const int16_t silence[800];
    (void)bsp_audio_write(silence, sizeof(silence));
}

static void voice_task(void *arg) {
    (void)arg;
    if (bsp_audio_init() != ESP_OK ||
        bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(s_volume);
    s_hz = 16000;
    bool ok = true;
    const bool hidden = archery_challenge_main_is_hidden(s_main_id);
    if (hidden) {
        (void)bsp_audio_set_format(16000, 16, 1);
        s_hz = 16000;
        ok = write_all(&s_intro, 16000);
    }
    if (ok && s_main_id < ARCHERY_CHALLENGE_MAIN_COUNT) {
        ok = write_all(&s_main[s_main_id], 16000);
    }
    if (ok && s_mod_count > 0u) {
        write_silence();
        for (uint8_t i = 0; i < s_mod_count && ok; i++) {
            if (s_mod_ids[i] < ARCHERY_CHALLENGE_MOD_COUNT) {
                ok = write_all(&s_mod[s_mod_ids[i]], 16000);
                if (ok && i + 1u < s_mod_count) write_silence();
            }
        }
    }
    if (ok && s_difficulty >= 1u && s_difficulty <= 15u) {
        write_silence();
        (void)write_all(&s_diff[s_difficulty - 1u], 16000);
    }
    (void)bsp_audio_set_format(16000, 16, 1);
    s_task = NULL;
    vTaskDelete(NULL);
}

static void cue_task(void *arg) {
    (void)arg;
    if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
        s_cue_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(s_volume);
    const uint8_t *p = s_cue.start;
    while (p < s_cue.end && !s_cue_stop) {
        size_t chunk = (size_t)(s_cue.end - p);
        if (chunk > 2048u) chunk = 2048u;
        if (bsp_audio_write(p, chunk) != ESP_OK) break;
        p += chunk;
    }
    s_cue_task = NULL;
    vTaskDelete(NULL);
}

void archery_challenge_voice_play_cue(void) {
    if (s_cue_task) return;
    s_cue_stop = false;
    if (xTaskCreate(cue_task, "challenge_cue", 4096, NULL, 3, &s_cue_task) != pdPASS) {
        s_cue_task = NULL;
    }
}

void archery_challenge_voice_stop_cue(void) {
    s_cue_stop = true;
}

void archery_challenge_voice_stop(void) {
    s_stop = true;
}

void archery_challenge_voice_play(uint8_t main_id, const uint8_t *mod_ids,
                                  uint8_t mod_count, uint8_t difficulty,
                                  uint8_t volume_percent) {
    if (main_id >= ARCHERY_CHALLENGE_MAIN_COUNT) return;
    archery_challenge_voice_stop();
    archery_challenge_voice_stop_cue();
    if (s_task) vTaskDelay(pdMS_TO_TICKS(120));
    if (s_cue_task) vTaskDelay(pdMS_TO_TICKS(80));
    s_main_id = main_id;
    s_mod_count = mod_count > ARCHERY_CHALLENGE_MAX_MODS
                      ? ARCHERY_CHALLENGE_MAX_MODS : mod_count;
    for (uint8_t i = 0; i < s_mod_count; i++) s_mod_ids[i] = mod_ids[i];
    s_difficulty = difficulty;
    s_volume = volume_percent > 100u ? 100u : volume_percent;
    s_stop = false;
    if (xTaskCreate(voice_task, "challenge_voice", 8192, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
    }
}
