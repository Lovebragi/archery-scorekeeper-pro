// tests/test_menu_navigation.c —— 菜单循环与旋转映射契约。
#include "archery_flow.h"

#include <assert.h>

static void press(archery_flow_t *flow, archery_key_t key) {
    (void)archery_flow_handle(flow, key);
}

static void test_home_wrap(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == ARCHERY_HOME_ITEMS - 1u);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 0);
}

static void test_tools_wrap(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    flow.page = ARCHERY_PAGE_TOOLS;
    flow.tools_row = 0;
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.tools_row == 4);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.tools_row == 0);
}

static void test_sight_wrap(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    flow.page = ARCHERY_PAGE_SIGHT;
    flow.sight_row = 0;
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.sight_row == flow.sight.count - 1u);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.sight_row == 0);
}

static void test_settings_wrap(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    flow.page = ARCHERY_PAGE_SETTINGS;
    flow.settings_row = 0;
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.settings_row == ARCHERY_SETTINGS_ROWS - 1u);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 0);
}

static void test_sight_estimate(void) {
    archery_sight_t sight;
    archery_sight_defaults(&sight);
    const int16_t marks[] = {0, 60, 160, 260};  // 线性：5 tenths/m
    for (uint8_t i = 0; i < 4; i++) sight.items[i].mark_tenths = marks[i];
    int16_t mark = -1;
    assert(archery_sight_estimate(&sight, 400, ARCHERY_SIGHT_LINEAR, &mark));
    assert(mark == 110);
    assert(archery_sight_estimate(&sight, 400, ARCHERY_SIGHT_QUADRATIC, &mark));
    assert(mark == 110);
    assert(!archery_sight_estimate(&sight, 100, ARCHERY_SIGHT_LINEAR, &mark));
    assert(!archery_sight_estimate(&sight, 800, ARCHERY_SIGHT_LINEAR, &mark));
    assert(archery_sight_estimate(&sight, 800, ARCHERY_SIGHT_QUADRATIC, &mark));
    assert(mark == 310);
}

static void test_sight_validity(void) {
    archery_sight_t sight;
    archery_sight_defaults(&sight);
    sight.items[0].mark_tenths = 120;  // 18m 1.2
    sight.items[1].mark_tenths = 110;  // 30m 1.1：距离更远刻度更低
    assert(!archery_sight_entry_valid(&sight, 0));
    assert(!archery_sight_entry_valid(&sight, 1));
    sight.items[1].mark_tenths = 130;
    assert(archery_sight_entry_valid(&sight, 0));
    assert(archery_sight_entry_valid(&sight, 1));
}

static void test_rotate_mapping(void) {
    assert(archery_flow_button_key(ARCHERY_BUTTON_UP, false) == ARCHERY_KEY_UP);
    assert(archery_flow_button_key(ARCHERY_BUTTON_DOWN, false) == ARCHERY_KEY_DOWN);
    assert(archery_flow_button_key(ARCHERY_BUTTON_OK, false) == ARCHERY_KEY_OK);

    assert(archery_flow_button_key(ARCHERY_BUTTON_UP, true) == ARCHERY_KEY_OK);
    assert(archery_flow_button_key(ARCHERY_BUTTON_DOWN, true) == ARCHERY_KEY_DOWN);
    assert(archery_flow_button_key(ARCHERY_BUTTON_OK, true) == ARCHERY_KEY_UP);
}

int main(void) {
    test_home_wrap();
    test_tools_wrap();
    test_sight_wrap();
    test_settings_wrap();
    test_sight_estimate();
    test_sight_validity();
    test_rotate_mapping();
    return 0;
}
