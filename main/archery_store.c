// main/archery_store.c —— 历史记录的 NVS 读写实现。
#include "archery_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "archery_store";
static const char *NVS_NAMESPACE = "archery";
static const char *META_KEY = "meta";
static const char *SETTINGS_KEY = "cfg";
static const char *CLOCK_KEY = "clk";
static const char *SCHEMA_KEY = "schema";
#define STORE_SCHEMA 2u
#define CLOCK_MAGIC 0xC1u

static void slot_key(char *buffer, size_t size, uint16_t slot) {
    snprintf(buffer, size, "s%03u", (unsigned)slot);
}

static esp_err_t open_nvs(nvs_handle_t *handle) {
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
}

static esp_err_t write_meta(nvs_handle_t handle, const archery_history_meta_t *meta) {
    return nvs_set_blob(handle, META_KEY, meta, sizeof(*meta));
}

static void history_clear_memory(archery_history_t *history) {
    memset(history, 0, sizeof(*history));
    archery_history_meta_init(&history->meta);
}

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

// 分区表扩到 256 KB、记录格式升到 v3 后，旧 NVS 内容不再可靠：首次启动时
// 整体清空一次，之后靠 schema 标记判断。
static esp_err_t ensure_schema(nvs_handle_t handle) {
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, SCHEMA_KEY, &value);
    if (err == ESP_OK && value == STORE_SCHEMA) return ESP_OK;
    ESP_LOGW(TAG, "存储布局升级，清空一次 NVS（历史/设置/时钟将重置）");
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, SCHEMA_KEY, STORE_SCHEMA);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "初始化存储布局失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t archery_store_load(archery_history_t *history) {
    if (!history) return ESP_ERR_INVALID_ARG;
    history_clear_memory(history);

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间失败: %s", esp_err_to_name(err));
        return err;
    }
    err = ensure_schema(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    // meta 缺失或长度不符时按空历史处理，后面的扫描会重新推算 next_seq。
    archery_history_meta_t meta;
    size_t meta_size = sizeof(meta);
    err = nvs_get_blob(handle, META_KEY, &meta, &meta_size);
    bool meta_ok = err == ESP_OK && meta_size == sizeof(meta) &&
                   meta.next_seq >= 1 &&
                   meta.count <= ARCHERY_HISTORY_CAPACITY;
    if (!meta_ok) {
        archery_history_meta_init(&meta);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "读取 meta 失败(%s)，按空历史处理", esp_err_to_name(err));
        }
    }

    uint8_t buffer[ARCHERY_RECORD_BUFFER_BYTES];
    uint16_t max_seq = 0;
    for (uint16_t slot = 0; slot < ARCHERY_HISTORY_CAPACITY; slot++) {
        char key[8];
        slot_key(key, sizeof(key), slot);
        size_t length = sizeof(buffer);
        esp_err_t read_err = nvs_get_blob(handle, key, buffer, &length);
        archery_record_t record;
        if (read_err == ESP_OK && archery_record_decode(&record, buffer, length)) {
            history->records[slot] = record;
            if (record.seq > max_seq) max_seq = record.seq;
        } else {
            history->records[slot].seq = 0;
            if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "槽位 %u 读取失败: %s", (unsigned)slot,
                         esp_err_to_name(read_err));
            } else if (read_err == ESP_OK) {
                ESP_LOGW(TAG, "槽位 %u 记录校验失败，已忽略", (unsigned)slot);
            }
        }
    }

    bool dirty = !meta_ok;
    if (meta.next_seq <= max_seq) {
        meta.next_seq = (uint16_t)(max_seq + 1u);
        dirty = true;
    }

    // 从最新一条向前数出连续可用的记录数；遇到缺口即视为更早的记录已不可用。
    uint16_t count = 0;
    for (uint16_t i = 0; i < ARCHERY_HISTORY_CAPACITY; i++) {
        const uint16_t seq = archery_history_seq_newest(&meta, i);
        if (seq == 0) break;
        const uint16_t slot = archery_history_slot_for_seq(seq);
        if (slot == ARCHERY_HISTORY_SLOT_INVALID || history->records[slot].seq != seq) break;
        count++;
    }
    if (meta.count != count) {
        meta.count = count;
        dirty = true;
    }

    history->meta = meta;
    if (dirty) {
        esp_err_t write_err = write_meta(handle, &meta);
        if (write_err == ESP_OK) write_err = nvs_commit(handle);
        if (write_err != ESP_OK) {
            ESP_LOGW(TAG, "修复 meta 写回失败: %s", esp_err_to_name(write_err));
        }
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "历史记录载入 %u 条，下一条序号 %u", (unsigned)count,
             (unsigned)meta.next_seq);
    return ESP_OK;
}

esp_err_t archery_store_save(archery_history_t *history, const archery_record_t *record) {
    if (!history || !record) return ESP_ERR_INVALID_ARG;
    if (history->meta.next_seq == 0) return ESP_ERR_INVALID_STATE;

    archery_record_t stored = *record;
    stored.seq = history->meta.next_seq;

    uint8_t buffer[ARCHERY_RECORD_BUFFER_BYTES];
    const size_t length = archery_record_encode(&stored, buffer, sizeof(buffer));
    if (length == 0) return ESP_ERR_INVALID_ARG;

    const uint16_t slot = archery_history_slot_for_seq(stored.seq);
    if (slot == ARCHERY_HISTORY_SLOT_INVALID) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) return err;

    char key[8];
    slot_key(key, sizeof(key), slot);
    err = nvs_set_blob(handle, key, buffer, length);
    if (err == ESP_OK) {
        archery_history_meta_t meta = history->meta;
        archery_history_meta_after_save(&meta);
        err = write_meta(handle, &meta);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err == ESP_OK) {
            history->records[slot] = stored;
            history->meta = meta;
        }
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入记录失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "记录 #%u 已保存: %u 组 × %u 箭", (unsigned)stored.seq,
             (unsigned)stored.groups, (unsigned)stored.arrows);
    return ESP_OK;
}

esp_err_t archery_store_clear(archery_history_t *history) {
    if (!history) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) return err;
    // 只删记录与 meta，保留设置、时钟和 schema 标记。
    char key[8];
    for (uint16_t slot = 0; slot < ARCHERY_HISTORY_CAPACITY; slot++) {
        slot_key(key, sizeof(key), slot);
        const esp_err_t erase_err = nvs_erase_key(handle, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND && err == ESP_OK) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        archery_history_meta_t meta;
        archery_history_meta_init(&meta);
        err = write_meta(handle, &meta);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err == ESP_OK) history_clear_memory(history);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "清空历史失败: %s", esp_err_to_name(err));
    }
    return err;
}

const archery_record_t *archery_history_record(const archery_history_t *history,
                                               uint16_t newest_index) {
    if (!history) return NULL;
    const uint16_t seq = archery_history_seq_newest(&history->meta, newest_index);
    if (seq == 0) return NULL;
    const uint16_t slot = archery_history_slot_for_seq(seq);
    if (slot == ARCHERY_HISTORY_SLOT_INVALID) return NULL;
    const archery_record_t *record = &history->records[slot];
    return record->seq == seq ? record : NULL;
}

uint16_t archery_history_visible_count(const archery_history_t *history,
                                       uint8_t filter, uint32_t now) {
    if (!history) return 0;
    uint16_t count = 0;
    for (uint16_t i = 0; i < history->meta.count; i++) {
        const archery_record_t *record = archery_history_record(history, i);
        if (record && archery_clock_matches_filter(record->when, filter, now)) count++;
    }
    return count;
}

const archery_record_t *archery_history_visible_record(const archery_history_t *history,
                                                       uint8_t filter, uint32_t now,
                                                       uint16_t index) {
    if (!history) return NULL;
    uint16_t matched = 0;
    for (uint16_t i = 0; i < history->meta.count; i++) {
        const archery_record_t *record = archery_history_record(history, i);
        if (!record || !archery_clock_matches_filter(record->when, filter, now)) continue;
        if (matched == index) return record;
        matched++;
    }
    return NULL;
}

esp_err_t archery_store_clock_load(uint32_t *seconds, bool *time_set) {
    if (!seconds || !time_set) return ESP_ERR_INVALID_ARG;
    *seconds = 0;
    *time_set = false;

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) return err;
    uint8_t buffer[9];
    size_t length = sizeof(buffer);
    err = nvs_get_blob(handle, CLOCK_KEY, buffer, &length);
    if (err == ESP_OK && length == sizeof(buffer) && buffer[0] == CLOCK_MAGIC) {
        const uint16_t expected = (uint16_t)(buffer[7] | ((uint16_t)buffer[8] << 8));
        if (crc16_ccitt(buffer, 7) == expected) {
            *seconds = (uint32_t)buffer[2] | ((uint32_t)buffer[3] << 8) |
                       ((uint32_t)buffer[4] << 16) | ((uint32_t)buffer[5] << 24);
            *time_set = (buffer[1] & 0x01u) != 0;
        }
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t archery_store_clock_save(uint32_t seconds, bool time_set) {
    uint8_t buffer[9];
    buffer[0] = CLOCK_MAGIC;
    buffer[1] = time_set ? 0x01u : 0x00u;
    buffer[2] = (uint8_t)(seconds & 0xFFu);
    buffer[3] = (uint8_t)((seconds >> 8) & 0xFFu);
    buffer[4] = (uint8_t)((seconds >> 16) & 0xFFu);
    buffer[5] = (uint8_t)((seconds >> 24) & 0xFFu);
    buffer[6] = 0;
    const uint16_t crc = crc16_ccitt(buffer, 7);
    buffer[7] = (uint8_t)(crc & 0xFFu);
    buffer[8] = (uint8_t)(crc >> 8);

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, CLOCK_KEY, buffer, sizeof(buffer));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t archery_settings_load(archery_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    archery_settings_defaults(settings);

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 读取设置失败(%s)，使用默认值", esp_err_to_name(err));
        return err;
    }
    uint8_t buffer[ARCHERY_SETTINGS_ENCODED_BYTES];
    size_t length = sizeof(buffer);
    err = nvs_get_blob(handle, SETTINGS_KEY, buffer, &length);
    archery_settings_t loaded;
    if (err == ESP_OK && archery_settings_decode(&loaded, buffer, length)) {
        archery_settings_normalize(&loaded);
        *settings = loaded;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "设置数据不可用(%s)，使用默认值", esp_err_to_name(err));
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t archery_settings_save(const archery_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    uint8_t buffer[ARCHERY_SETTINGS_ENCODED_BYTES];
    const size_t length = archery_settings_encode(settings, buffer, sizeof(buffer));
    if (length == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = open_nvs(&handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, SETTINGS_KEY, buffer, length);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存设置失败: %s", esp_err_to_name(err));
    }
    return err;
}
