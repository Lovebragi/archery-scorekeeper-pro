// main/archery_sight.c —— 瞄具刻度表的默认值与 NVS 持久化。
#include "archery_sight.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

#define SIGHT_NAMESPACE "archery"
#define SIGHT_KEY "sight"
#define SIGHT_MAGIC 0xA5u
#define SIGHT_VERSION 2u

void archery_sight_defaults(archery_sight_t *sight) {
    if (!sight) return;
    memset(sight, 0, sizeof(*sight));
    static const uint16_t distances[] = {180, 300, 500, 700, 0, 0};
    sight->count = (uint8_t)(sizeof(distances) / sizeof(distances[0]));
    for (uint8_t i = 0; i < sight->count; i++) {
        sight->items[i].distance_tenths = distances[i];
        sight->items[i].mark_tenths = 0;
    }
}

bool archery_sight_load(archery_sight_t *sight) {
    if (!sight) return false;
    archery_sight_defaults(sight);
#ifndef ESP_PLATFORM
    return false;
#else
    nvs_handle_t handle;
    if (nvs_open(SIGHT_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t buffer[3u + ARCHERY_SIGHT_MAX * 4u];
    size_t length = sizeof(buffer);
    const esp_err_t err = nvs_get_blob(handle, SIGHT_KEY, buffer, &length);
    nvs_close(handle);
    if (err != ESP_OK || length < 3u || buffer[0] != SIGHT_MAGIC ||
        (buffer[1] != 1u && buffer[1] != SIGHT_VERSION)) {
        return false;
    }
    const bool legacy_meters = buffer[1] == 1u;
    const uint8_t count = buffer[2];
    if (count == 0 || count > ARCHERY_SIGHT_MAX || length != 3u + count * 4u) {
        return false;
    }
    sight->count = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = buffer + 3u + i * 4u;
        sight->items[i].distance_tenths = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        if (legacy_meters) sight->items[i].distance_tenths *= 10u;
        sight->items[i].mark_tenths = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
    }
    // 旧数据只有 4 行时补两行自定义距离。
    for (uint8_t i = count; i < 6u && i < ARCHERY_SIGHT_MAX; i++) {
        sight->items[i].distance_tenths = 0;
        sight->items[i].mark_tenths = 0;
    }
    if (sight->count < 6u) sight->count = 6u;
    return true;
#endif
}

bool archery_sight_save(const archery_sight_t *sight) {
    if (!sight || sight->count == 0 || sight->count > ARCHERY_SIGHT_MAX) return false;
#ifndef ESP_PLATFORM
    return false;
#else
    uint8_t buffer[3u + ARCHERY_SIGHT_MAX * 4u];
    buffer[0] = SIGHT_MAGIC;
    buffer[1] = SIGHT_VERSION;
    buffer[2] = sight->count;
    size_t used = 3u;
    for (uint8_t i = 0; i < sight->count; i++) {
        buffer[used++] = (uint8_t)(sight->items[i].distance_tenths & 0xFFu);
        buffer[used++] = (uint8_t)(sight->items[i].distance_tenths >> 8);
        buffer[used++] = (uint8_t)((uint16_t)sight->items[i].mark_tenths & 0xFFu);
        buffer[used++] = (uint8_t)((uint16_t)sight->items[i].mark_tenths >> 8);
    }
    nvs_handle_t handle;
    if (nvs_open(SIGHT_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, SIGHT_KEY, buffer, used);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
#endif
}

static double lagrange(double x, const double *xs, const double *ys, int n) {
    double total = 0.0;
    for (int i = 0; i < n; i++) {
        double term = ys[i];
        for (int j = 0; j < n; j++) {
            if (i != j) term *= (x - xs[j]) / (xs[i] - xs[j]);
        }
        total += term;
    }
    return total;
}

bool archery_sight_entry_valid(const archery_sight_t *sight, uint8_t index) {
    if (!sight || index >= sight->count) return false;
    const uint16_t dist = sight->items[index].distance_tenths;
    const int16_t mark = sight->items[index].mark_tenths;
    if (dist == 0 || mark == 0) return false;
    for (uint8_t i = 0; i < sight->count; i++) {
        if (i == index) continue;
        const uint16_t od = sight->items[i].distance_tenths;
        const int16_t om = sight->items[i].mark_tenths;
        if (od == 0 || om == 0) continue;
        if (od == dist) return false;
        if (od < dist && om >= mark) return false;
        if (od > dist && om <= mark) return false;
    }
    return true;
}

bool archery_sight_estimate(const archery_sight_t *sight, uint16_t distance_tenths,
                            archery_sight_fit_t fit, int16_t *mark_tenths) {
    if (!sight || !mark_tenths) return false;

    // 0.0 视为“未设置”，不参与插值/外推。
    uint8_t valid[ARCHERY_SIGHT_MAX];
    uint8_t n = 0;
    for (uint8_t i = 0; i < sight->count; i++) {
        if (archery_sight_entry_valid(sight, i)) valid[n++] = i;
    }
    if (n < 2) return false;

    for (uint8_t k = 0; k < n; k++) {
        if (sight->items[valid[k]].distance_tenths == distance_tenths) {
            *mark_tenths = sight->items[valid[k]].mark_tenths;
            return true;
        }
    }

    uint8_t lower = 0xFF, upper = 0xFF;
    for (uint8_t k = 0; k < n; k++) {
        const uint8_t i = valid[k];
        if (sight->items[i].distance_tenths < distance_tenths &&
            (lower == 0xFF || sight->items[i].distance_tenths > sight->items[lower].distance_tenths)) {
            lower = i;
        }
        if (sight->items[i].distance_tenths > distance_tenths &&
            (upper == 0xFF || sight->items[i].distance_tenths < sight->items[upper].distance_tenths)) {
            upper = i;
        }
    }

    double result;
    if (fit == ARCHERY_SIGHT_QUADRATIC) {
        if (lower != 0xFF && upper != 0xFF && n >= 3) {
            // 内插：两点 + 最近的第三点。
            uint8_t third = 0xFF;
            int best = 0x7FFFFFFF;
            for (uint8_t k = 0; k < n; k++) {
                const uint8_t i = valid[k];
                if (i == lower || i == upper) continue;
                const int d = (int)sight->items[i].distance_tenths - (int)distance_tenths;
                const int ad = d < 0 ? -d : d;
                if (ad < best) { best = ad; third = i; }
            }
            if (third == 0xFF) return false;
            const double xs[3] = {sight->items[lower].distance_tenths,
                                  sight->items[upper].distance_tenths,
                                  sight->items[third].distance_tenths};
            const double ys[3] = {sight->items[lower].mark_tenths,
                                  sight->items[upper].mark_tenths,
                                  sight->items[third].mark_tenths};
            result = lagrange((double)distance_tenths, xs, ys, 3);
        } else if (lower != 0xFF) {
            // 超过最大有效刻度：用最后两个有效点的斜率。
            const uint8_t a = valid[n - 2u], b = valid[n - 1u];
            const double x0 = sight->items[a].distance_tenths;
            const double x1 = sight->items[b].distance_tenths;
            const double y0 = sight->items[a].mark_tenths;
            const double y1 = sight->items[b].mark_tenths;
            result = y1 + (y1 - y0) * ((double)distance_tenths - x1) / (x1 - x0);
        } else if (upper != 0xFF) {
            // 低于最小有效刻度：用最前两个有效点的斜率。
            const uint8_t a = valid[0], b = valid[1];
            const double x0 = sight->items[a].distance_tenths;
            const double x1 = sight->items[b].distance_tenths;
            const double y0 = sight->items[a].mark_tenths;
            const double y1 = sight->items[b].mark_tenths;
            result = y0 + (y1 - y0) * ((double)distance_tenths - x0) / (x1 - x0);
        } else {
            return false;
        }
    } else {
        if (lower == 0xFF || upper == 0xFF) return false;
        const double x0 = sight->items[lower].distance_tenths;
        const double x1 = sight->items[upper].distance_tenths;
        const double y0 = sight->items[lower].mark_tenths;
        const double y1 = sight->items[upper].mark_tenths;
        result = y0 + (y1 - y0) * ((double)distance_tenths - x0) / (x1 - x0);
    }

    if (result < 0) result = 0;
    if (result > 9999) result = 9999;
    *mark_tenths = (int16_t)(result + 0.5);
    return true;
}

void archery_sight_format(int16_t mark_tenths, char *buffer, unsigned capacity) {
    if (!buffer || capacity == 0) return;
    const int value = mark_tenths;
    const int whole = value / 10;
    int frac = value % 10;
    if (frac < 0) frac = -frac;
    snprintf(buffer, capacity, "%d.%d", whole, frac);
}
