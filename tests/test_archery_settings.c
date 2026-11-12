// tests/test_archery_settings.c —— 靶型/方向/亮度/息屏时间的选项、边界与编解码测试。
#include "archery_settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t crc16_ccitt(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void test_defaults_and_valid(void) {
    archery_settings_t settings;
    archery_settings_defaults(&settings);
    assert(settings.target == ARCHERY_TARGET_HALF_40);
    assert(settings.rotate == ARCHERY_SETTINGS_DEFAULT_ROTATE);
    assert(settings.rotate == 1);
    assert(settings.brightness == ARCHERY_SETTINGS_DEFAULT_BRIGHTNESS);
    assert(settings.off_seconds == ARCHERY_SETTINGS_DEFAULT_OFF_SECONDS);
    assert(settings.record_x == 0);
    assert(archery_settings_valid(&settings));
    assert(!archery_settings_valid(NULL));

    settings.target = ARCHERY_TARGET_COUNT;
    assert(!archery_settings_valid(&settings));
    settings.target = ARCHERY_TARGET_HALF_40;
    settings.rotate = 2;
    assert(!archery_settings_valid(&settings));
    settings.rotate = 1;
    settings.brightness = 55;
    assert(!archery_settings_valid(&settings));
    settings.brightness = 100;
    settings.off_seconds = 45;
    assert(!archery_settings_valid(&settings));
    settings.off_seconds = 15;
    settings.record_x = 2;
    assert(!archery_settings_valid(&settings));
}

static void test_option_tables(void) {
    assert(archery_settings_target_count() == ARCHERY_TARGET_COUNT);
    assert(archery_settings_target_at(0) == ARCHERY_TARGET_HALF_40);
    assert(archery_settings_target_at(1) == ARCHERY_TARGET_TRIPLE_40H);
    assert(archery_settings_target_at(4) == ARCHERY_TARGET_FULL_122);
    assert(archery_settings_brightness_count() == 5);
    assert(archery_settings_brightness_at(4) == 100);
    assert(archery_settings_off_count() == 5);
    assert(archery_settings_off_at(0) == 0);
    assert(archery_settings_off_at(4) == 120);
}

static void test_adjust(void) {
    archery_settings_t settings;
    archery_settings_defaults(&settings);  // 40 半靶 / 倒转 / 100% / 30s

    // 靶型
    assert(archery_settings_adjust(&settings, 0, 1));
    assert(settings.target == ARCHERY_TARGET_TRIPLE_40H);
    assert(archery_settings_adjust(&settings, 0, 3));
    assert(settings.target == ARCHERY_TARGET_FULL_122);
    assert(!archery_settings_adjust(&settings, 0, 1));
    assert(archery_settings_adjust(&settings, 0, -4));
    assert(settings.target == ARCHERY_TARGET_HALF_40);

    // 屏幕方向：任意方向调整都相当于切换
    assert(archery_settings_adjust(&settings, 1, 1));
    assert(settings.rotate == 0);
    assert(archery_settings_adjust(&settings, 1, -1));
    assert(settings.rotate == 1);

    // 亮度：默认 20（最低），向下无效，向上到 40、60
    assert(!archery_settings_adjust(&settings, 2, -1));
    assert(settings.brightness == ARCHERY_SETTINGS_DEFAULT_BRIGHTNESS);
    assert(archery_settings_adjust(&settings, 2, 1));
    assert(settings.brightness == 40);
    assert(archery_settings_adjust(&settings, 2, 1));
    assert(settings.brightness == 60);

    // 息屏时间：默认 0（不息屏）-> 15
    assert(archery_settings_adjust(&settings, 3, 1));
    assert(settings.off_seconds == 15);
    settings.off_seconds = 0;
    assert(!archery_settings_adjust(&settings, 3, -1));
    assert(settings.off_seconds == 0);

    // 记录内十：默认关，任一方向切换
    assert(archery_settings_adjust(&settings, 4, 1));
    assert(settings.record_x == 1);
    assert(archery_settings_adjust(&settings, 4, -1));
    assert(settings.record_x == 0);

    // 每组计时：关 -> 20 -> 30 -> 40，边界钳制
    assert(settings.timer_seconds == 0);
    assert(archery_settings_adjust(&settings, 5, 1));
    assert(settings.timer_seconds == 20);
    assert(archery_settings_adjust(&settings, 5, 1));
    assert(settings.timer_seconds == 30);
    assert(archery_settings_adjust(&settings, 5, 1));
    assert(settings.timer_seconds == 40);
    assert(!archery_settings_adjust(&settings, 5, 1));
    assert(archery_settings_adjust(&settings, 5, -1));
    assert(settings.timer_seconds == 30);

    // 倒数十秒语音：豆包 -> 关 -> 系统 -> 小豆 -> 豆包
    assert(settings.countdown_voice == 3);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 4);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 5);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 6);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 7);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 0);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 1);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 2);
    assert(archery_settings_adjust(&settings, 6, 1));
    assert(settings.countdown_voice == 3);

    // 语音输入：默认开，任一方向切换
    assert(archery_settings_adjust(&settings, 7, 1));
    assert(settings.voice_enabled == 0);
    assert(archery_settings_adjust(&settings, 7, -1));
    assert(settings.voice_enabled == 1);

    // 滚动录入：默认关，任一方向切换
    assert(settings.roll_entry == 0);
    assert(archery_settings_adjust(&settings, 8, 1));
    assert(settings.roll_entry == 1);
    assert(archery_settings_adjust(&settings, 8, -1));
    assert(settings.roll_entry == 0);

    // 提示音量：默认 60%（档位 3），可到 0（静音）和 5（100%）
    assert(settings.beep_volume == 3);
    assert(archery_settings_adjust(&settings, 9, 1));
    assert(settings.beep_volume == 4);
    assert(archery_settings_adjust(&settings, 9, -4));
    assert(settings.beep_volume == 0);
    assert(!archery_settings_adjust(&settings, 9, -1));
    assert(archery_settings_adjust(&settings, 9, 1));
    assert(settings.beep_volume == 1);

    // 保存 Wi-Fi 密码：默认开，任一方向切换
    assert(settings.wifi_save == 1);
    assert(archery_settings_adjust(&settings, 10, 1));
    assert(settings.wifi_save == 0);
    assert(archery_settings_adjust(&settings, 10, -1));
    assert(settings.wifi_save == 1);

    // 非法行（没有第 9 行）
    assert(!archery_settings_adjust(&settings, 12, 1));
    assert(!archery_settings_adjust(NULL, 0, 1));
}

static void test_encode_decode(void) {
    archery_settings_t settings;
    archery_settings_defaults(&settings);
    settings.target = ARCHERY_TARGET_FULL_80;
    settings.rotate = 0;
    settings.brightness = 60;
    settings.off_seconds = 120;
    settings.record_x = 0;

    uint8_t buffer[ARCHERY_SETTINGS_ENCODED_BYTES];
    assert(archery_settings_encode(&settings, buffer, sizeof(buffer)) ==
           ARCHERY_SETTINGS_ENCODED_BYTES);

    archery_settings_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    assert(archery_settings_decode(&decoded, buffer, sizeof(buffer)));
    assert(decoded.target == ARCHERY_TARGET_FULL_80);
    assert(decoded.rotate == 0);
    assert(decoded.brightness == 60);
    assert(decoded.off_seconds == 120);
    assert(decoded.record_x == 0);

    assert(archery_settings_encode(&settings, buffer, 4) == 0);
    assert(!archery_settings_decode(&decoded, buffer, 9));

    uint8_t copy[ARCHERY_SETTINGS_ENCODED_BYTES];
    memcpy(copy, buffer, sizeof(copy));
    copy[0] ^= 0xFF;
    assert(!archery_settings_decode(&decoded, copy, sizeof(copy)));
    memcpy(copy, buffer, sizeof(copy));
    copy[1] = 12;
    assert(!archery_settings_decode(&decoded, copy, sizeof(copy)));
    memcpy(copy, buffer, sizeof(copy));
    copy[9] ^= 0x01;
    assert(!archery_settings_decode(&decoded, copy, sizeof(copy)));

    // 字段非法（方向 2）即使 CRC 正确也拒绝
    memcpy(copy, buffer, sizeof(copy));
    copy[3] = 2;
    const uint16_t crc = crc16_ccitt(copy, 8);
    copy[8] = (uint8_t)(crc & 0xFFu);
    copy[9] = (uint8_t)(crc >> 8);
    assert(!archery_settings_decode(&decoded, copy, sizeof(copy)));
}

// 旧版 v1（8 字节，无靶型）与 v2（9 字节，无方向）按默认值补齐。
static void test_legacy_compatibility(void) {
    uint8_t old[ARCHERY_SETTINGS_ENCODED_BYTES_V2];
    old[0] = 0x5A;
    old[1] = 1;    // v1
    old[2] = 40;   // 亮度
    old[3] = 30;   // 息屏 lo
    old[4] = 0;    // 息屏 hi
    old[5] = 0;    // 保留
    uint16_t crc = crc16_ccitt(old, 6);
    old[6] = (uint8_t)(crc & 0xFFu);
    old[7] = (uint8_t)(crc >> 8);
    archery_settings_t decoded;
    assert(archery_settings_decode(&decoded, old, ARCHERY_SETTINGS_ENCODED_BYTES_V1));
    assert(decoded.target == ARCHERY_TARGET_HALF_40);
    assert(decoded.rotate == ARCHERY_SETTINGS_DEFAULT_ROTATE);
    assert(decoded.brightness == 40);
    assert(decoded.off_seconds == 30);
    assert(decoded.record_x == 1);

    uint8_t v2[ARCHERY_SETTINGS_ENCODED_BYTES_V2];
    v2[0] = 0x5A;
    v2[1] = 2;    // v2
    v2[2] = ARCHERY_TARGET_FULL_40;
    v2[3] = 80;   // 亮度
    v2[4] = 15;   // 息屏 lo
    v2[5] = 0;
    v2[6] = 0;
    crc = crc16_ccitt(v2, 7);
    v2[7] = (uint8_t)(crc & 0xFFu);
    v2[8] = (uint8_t)(crc >> 8);
    assert(archery_settings_decode(&decoded, v2, ARCHERY_SETTINGS_ENCODED_BYTES_V2));
    assert(decoded.target == ARCHERY_TARGET_FULL_40);
    assert(decoded.rotate == ARCHERY_SETTINGS_DEFAULT_ROTATE);
    assert(decoded.brightness == 80);
    assert(decoded.off_seconds == 15);
    assert(decoded.record_x == 1);

    // v3（10 字节，无"记录内十"字段）沿用旧行为：记录 X。
    uint8_t v3[ARCHERY_SETTINGS_ENCODED_BYTES_V4];
    v3[0] = 0x5A;
    v3[1] = 3;    // v3
    v3[2] = ARCHERY_TARGET_HALF_40;
    v3[3] = 1;    // 倒转
    v3[4] = 20;   // 亮度
    v3[5] = 15;   // 息屏 lo
    v3[6] = 0;
    v3[7] = 0;    // v3 的保留字节
    crc = crc16_ccitt(v3, 8);
    v3[8] = (uint8_t)(crc & 0xFFu);
    v3[9] = (uint8_t)(crc >> 8);
    assert(archery_settings_decode(&decoded, v3, sizeof(v3)));
    assert(decoded.target == ARCHERY_TARGET_HALF_40);
    assert(decoded.record_x == 1);

    // CRC 错误必须拒绝
    v2[8] ^= 0x01;
    assert(!archery_settings_decode(&decoded, v2, ARCHERY_SETTINGS_ENCODED_BYTES_V2));
}

int main(void) {
    test_defaults_and_valid();
    test_option_tables();
    test_adjust();
    test_encode_decode();
    test_legacy_compatibility();
    puts("archery settings tests: PASS");
    return 0;
}
