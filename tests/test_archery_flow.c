// tests/test_archery_flow.c —— 页面/按键状态机测试（按环值统计录入）。
#include "archery_flow.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void press(archery_flow_t *flow, archery_key_t key) {
    (void)archery_flow_handle(flow, key);
}

static void start_session(archery_flow_t *flow, int groups, int arrows) {
    // 旧用例按完整行数（含 X 行）编写，显式开启 X 行。
    flow->settings.record_x = 1;
    // 主页 -> 开始训练 -> 设置页
    flow->home_select = 0;
    press(flow, ARCHERY_KEY_OK);
    assert(flow->page == ARCHERY_PAGE_SETUP);
    for (int i = 0; i < 40; i++) press(flow, ARCHERY_KEY_DOWN);
    for (int i = 0; i < groups - 1; i++) press(flow, ARCHERY_KEY_UP);
    press(flow, ARCHERY_KEY_OK);  // 到"每组箭数"
    for (int i = 0; i < 40; i++) press(flow, ARCHERY_KEY_DOWN);
    for (int i = 0; i < arrows - 1; i++) press(flow, ARCHERY_KEY_UP);
    press(flow, ARCHERY_KEY_OK);  // 到"开始记录"
    assert(flow->setup_row == 2);
    assert(archery_flow_handle(flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SESSION_START);
    assert(flow->page == ARCHERY_PAGE_ENTRY);
    assert(flow->session.groups == groups && flow->session.arrows == arrows);
}

// 逐行下移，直到停在提交行（环值已满时会直接跳到提交行）。
static void goto_commit_row(archery_flow_t *flow) {
    const uint8_t commit = archery_target_slot_count(flow->session.target);
    while (flow->entry_row < commit) {
        press(flow, ARCHERY_KEY_OK);
    }
    assert(flow->entry_row == commit);
}

static void test_home_and_setup(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    assert(flow.page == ARCHERY_PAGE_HOME);
    assert(flow.setup_groups == ARCHERY_DEFAULT_GROUPS);
    assert(flow.setup_arrows == ARCHERY_DEFAULT_ARROWS);


    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 1);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 2);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 3);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 4);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 5);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 0);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == 5);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == 4);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == 3);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == 2);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == 1);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.home_select == 0);

    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SETUP);
    assert(flow.setup_row == 0);

    press(&flow, ARCHERY_KEY_UP);
    assert(flow.setup_groups == 7);
    for (int i = 0; i < 20; i++) press(&flow, ARCHERY_KEY_UP);
    assert(flow.setup_groups == ARCHERY_GROUPS_MAX);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.setup_row == 1);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.setup_arrows == 5);

    // 长按确定逐项回退，在首项再长按才回主页
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.setup_row == 0);
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_HOME);
}

static void test_ring_count_entry(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    flow.settings.record_x = 1;  // 本用例覆盖 X 行
    start_session(&flow, 2, 3);

    // 新一组：所有环值计数为 0，累计也从 0 开始
    assert(flow.entry_row == 0);
    assert(archery_counts_total(flow.counts[0]) == 0);
    assert(archery_session_total_points(&flow.session) == 0);

    // 第一行 X：上 -> 1
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][0] == 1);
    // 下 -> 0，再下仍为 0
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.counts[0][0] == 0);

    // 记 X1、10 1、9 1（共 3 根）
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_OK);   // 到 10 环行
    assert(flow.entry_row == 1);
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_OK);   // 到 9 环行
    press(&flow, ARCHERY_KEY_UP);
    assert(archery_counts_total(flow.counts[0]) == 3);

    // 已记满：继续按上不增加
    press(&flow, ARCHERY_KEY_UP);
    assert(archery_counts_total(flow.counts[0]) == 3);

    goto_commit_row(&flow);
    press(&flow, ARCHERY_KEY_OK);  // 合计正确，提交
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(flow.session.group == 1);
    assert(archery_session_total_points(&flow.session) == 29);
    assert(archery_session_group_x(&flow.session, 0) == 1);

    // 小结 -> 下一组：计数与光标复位
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    assert(flow.entry_row == 0);
    assert(archery_counts_total(flow.counts[0]) == 0);

    // 第二组只记 6 环 3 根
    for (int i = 0; i < 5; i++) press(&flow, ARCHERY_KEY_OK);  // 到 6 环行
    assert(flow.entry_row == 5);
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][5] == 3);
    goto_commit_row(&flow);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(archery_session_complete(&flow.session));
    assert(archery_session_total_points(&flow.session) == 29 + 18);

    // 最后一组提交后确定 -> 保存并进入完成页
    flow.save_failed = true;  // 该标记应在新一轮完成时清除
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SAVE_RECORD);
    assert(flow.page == ARCHERY_PAGE_FINISH);
    assert(!flow.save_failed);

    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HOME);
}

static void test_incomplete_group_cannot_commit(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    start_session(&flow, 1, 6);

    goto_commit_row(&flow);
    // 一根都没记：按确定不提交，而是回到第一行继续填
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    assert(flow.entry_row == 0);

    // 长按确定在环值行是"返回上一项"（首行时为放弃确认）
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_ABORT_CONFIRM);
    press(&flow, ARCHERY_KEY_OK);  // 继续训练
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    assert(flow.entry_row == 0);

    // 补齐 6 根（6 环行）后可以提交
    for (int i = 0; i < 5; i++) press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_row == 5);
    for (int i = 0; i < 6; i++) press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][5] == 6);
    goto_commit_row(&flow);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(archery_session_total_points(&flow.session) == 36);
}

static void test_abort_paths(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    start_session(&flow, 1, 2);

    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_ABORT_CONFIRM);
    assert(!flow.confirm_yes);
    // 默认"继续训练"：回到录入页
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    // 已填计数保留
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][0] == 1);

    // 选择"放弃本次训练"：回到主页
    press(&flow, ARCHERY_KEY_OK_LONG);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.confirm_yes);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HOME);

    // 小结页长按进入放弃确认，继续训练回到小结
    start_session(&flow, 1, 1);
    press(&flow, ARCHERY_KEY_UP);  // X 1 根
    goto_commit_row(&flow);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_ABORT_CONFIRM);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
}

static void test_button_role_rotation(void) {
    // 自然方向：物理上/下/确定就是逻辑上/下/确定
    assert(archery_flow_button_key(ARCHERY_BUTTON_UP, false) == ARCHERY_KEY_UP);
    assert(archery_flow_button_key(ARCHERY_BUTTON_DOWN, false) == ARCHERY_KEY_DOWN);
    assert(archery_flow_button_key(ARCHERY_BUTTON_OK, false) == ARCHERY_KEY_OK);
    assert(archery_flow_button_is_ok(ARCHERY_BUTTON_OK, false));
    assert(!archery_flow_button_is_ok(ARCHERY_BUTTON_UP, false));

    // 整机倒装：角色旋转 180°，物理上<->确定，下不变
    assert(archery_flow_button_key(ARCHERY_BUTTON_UP, true) == ARCHERY_KEY_OK);
    assert(archery_flow_button_key(ARCHERY_BUTTON_DOWN, true) == ARCHERY_KEY_DOWN);
    assert(archery_flow_button_key(ARCHERY_BUTTON_OK, true) == ARCHERY_KEY_UP);
    assert(archery_flow_button_is_ok(ARCHERY_BUTTON_UP, true));
    assert(!archery_flow_button_is_ok(ARCHERY_BUTTON_OK, true));
}

static void test_full_tally_shortcut(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    start_session(&flow, 1, 6);

    // 在 10 环行一次记满 6 根：确定应直接跳到提交行，而不是逐行下移
    press(&flow, ARCHERY_KEY_OK);  // 到 10 环行
    assert(flow.entry_row == 1);
    for (int i = 0; i < 6; i++) press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][1] == 6);
    assert(archery_counts_total(flow.counts[0]) == flow.session.arrows);

    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    assert(flow.entry_row == archery_target_slot_count(flow.session.target));

    // 提交行再按一次确定即提交
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(archery_session_total_points(&flow.session) == 60);
}

static void test_full_target_entry(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    archery_settings_t settings;
    archery_settings_defaults(&settings);
    settings.target = ARCHERY_TARGET_FULL_40;
    archery_flow_set_settings(&flow, &settings);

    start_session(&flow, 1, 2);
    assert(flow.session.target == ARCHERY_TARGET_FULL_40);
    assert(archery_target_slot_count(flow.session.target) == 12);

    // 第 1 槽是 10 环，记 1 根
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_row == 1);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][1] == 1);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.counts[0][1] == 0);
    press(&flow, ARCHERY_KEY_UP);

    // 走到第 11 槽（0 环），记 1 根；此时合计等于每组箭数
    for (int i = 0; i < 10; i++) press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_row == 11);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][11] == 1);
    assert(archery_counts_total(flow.counts[0]) == 2);

    // 记满后确定直接跳到提交行，再确定提交
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_row == 12);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(archery_session_group_points(&flow.session, 0) == 10);
    assert(archery_session_group_miss(&flow.session, 0) == 1);
}

static void test_entry_without_x(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    archery_settings_t settings;
    archery_settings_defaults(&settings);
    settings.target = ARCHERY_TARGET_FULL_40;
    settings.record_x = 0;  // 关闭"记录内十"：录入页从 10 环开始
    archery_flow_set_settings(&flow, &settings);

    start_session(&flow, 1, 2);
    flow.settings.record_x = 0;  // start_session 为旧用例强制开启 X，这里恢复关闭
    assert(flow.entry_row == 0);
    assert(archery_target_entry_first_slot(ARCHERY_TARGET_FULL_40, 0) == 1);
    assert(archery_target_entry_first_slot(ARCHERY_TARGET_FULL_40, 1) == 0);

    // 第 0 行就是 10 环（槽位 1），而不是 X（槽位 0）
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][1] == 1);
    assert(flow.counts[0][0] == 0);

    // 走到最后一个可见行（0 环，槽位 11）
    for (int i = 0; i < 10; i++) press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_row == 10);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.counts[0][11] == 1);
    assert(archery_counts_total(flow.counts[0]) == 2);

    // 记满跳到提交行（可见行号 11）并提交；X 槽位始终为 0
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_row == 11);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(archery_session_total_x(&flow.session) == 0);
    assert(archery_session_group_miss(&flow.session, 0) == 1);
}

static void test_triple_target_setup_and_entry(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    archery_settings_t settings;
    archery_settings_defaults(&settings);
    settings.target = ARCHERY_TARGET_TRIPLE_40H;
    settings.record_x = 1;  // 本用例按含 X 行的完整行数编写
    archery_flow_set_settings(&flow, &settings);

    // 主页 -> 新训练
    flow.home_select = 0;
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SETUP);
    assert(flow.setup_row == 0);
    assert(flow.setup_groups == 6 && flow.setup_arrows == 6);
    // 默认上/中/下 2/2/2
    assert(flow.setup_face[0] == 2 && flow.setup_face[1] == 2 && flow.setup_face[2] == 2);

    // 组数改成 1，每组箭数改成 4 -> 自动重分为 2/1/1
    for (int i = 0; i < 5; i++) press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.setup_groups == 1);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.setup_row == 1);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.setup_arrows == 4);
    assert(flow.setup_face[0] == 2 && flow.setup_face[1] == 1 && flow.setup_face[2] == 1);

    // 上靶改成 3 -> 合计 5 != 4，不允许开始
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.setup_row == 2);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.setup_face[0] == 3);
    assert(archery_flow_setup_faces_total(&flow) == 5);
    press(&flow, ARCHERY_KEY_OK);  // 中靶
    press(&flow, ARCHERY_KEY_OK);  // 下靶
    press(&flow, ARCHERY_KEY_OK);  // 开始
    assert(flow.setup_row == 5);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_NONE);
    assert(flow.page == ARCHERY_PAGE_SETUP);

    // 长按退回上靶行，改回 2 -> 合计 4
    press(&flow, ARCHERY_KEY_OK_LONG);
    press(&flow, ARCHERY_KEY_OK_LONG);
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.setup_row == 2);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.setup_face[0] == 2);
    assert(archery_flow_setup_faces_total(&flow) == 4);

    // 重新走到开始
    press(&flow, ARCHERY_KEY_OK);
    press(&flow, ARCHERY_KEY_OK);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.setup_row == 5);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SESSION_START);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
    assert(flow.session.target == ARCHERY_TARGET_TRIPLE_40H);
    assert(flow.session.arrows == 4);
    assert(flow.entry_face == 0);

    // 上靶 2 根：10 环 1 根、9 环 1 根
    press(&flow, ARCHERY_KEY_OK);  // X 行 -> 10 环行
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_OK);  // 9 环行
    press(&flow, ARCHERY_KEY_UP);
    assert(archery_counts_total(flow.counts[0]) == 2);
    press(&flow, ARCHERY_KEY_OK);  // 记满 -> 提交行
    assert(flow.entry_row == archery_target_slot_count(flow.session.target));
    // 提交上靶 -> 自动切到中靶
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.entry_face == 1);
    assert(flow.entry_row == 0);

    // 中靶 1 根：8 环
    for (int i = 0; i < 3; i++) press(&flow, ARCHERY_KEY_OK);  // 到 8 环行
    assert(flow.entry_row == 3);
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_OK);  // 记满 -> 提交行
    press(&flow, ARCHERY_KEY_OK);  // 提交中靶 -> 下靶
    assert(flow.entry_face == 2);

    // 下靶 1 根：脱靶
    for (int i = 0; i < 6; i++) press(&flow, ARCHERY_KEY_OK);  // 到脱靶行
    assert(flow.entry_row == 6);
    press(&flow, ARCHERY_KEY_UP);
    press(&flow, ARCHERY_KEY_OK);  // 记满 -> 提交行
    press(&flow, ARCHERY_KEY_OK);  // 提交整组
    assert(flow.page == ARCHERY_PAGE_SUMMARY);
    assert(flow.session.group == 1);
    // 10 + 9 + 8 + 0 = 27
    assert(archery_session_total_points(&flow.session) == 27);
    assert(archery_session_total_miss(&flow.session) == 1);
}

static void test_settings_page(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_SETTINGS);
    assert(flow.settings_row == 0);
    assert(flow.settings.target == ARCHERY_TARGET_HALF_40);

    // 确定循环当前行：半靶 -> 三联 -> 全靶 -> 半靶
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.target == ARCHERY_TARGET_TRIPLE_40H);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.target == ARCHERY_TARGET_FULL_40);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.target == ARCHERY_TARGET_FULL_80);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.target == ARCHERY_TARGET_FULL_122);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.target == ARCHERY_TARGET_HALF_40);

    // 上下只移动选中行，并在首尾循环
    assert(archery_flow_handle(&flow, ARCHERY_KEY_UP) == ARCHERY_ACT_NONE);
    assert(flow.settings_row == ARCHERY_SETTINGS_ROWS - 1);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_DOWN) == ARCHERY_ACT_NONE);
    assert(flow.settings_row == 0);

    // 方向：确定切换
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 1);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.rotate == 0);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.rotate == 1);

    // 亮度、息屏、记录内十、计时
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 2);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.brightness == 40);

    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 3);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.off_seconds == 15);

    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 4);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.record_x == 1);

    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 5);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.timer_seconds == 20);

    // 倒数十秒语音：豆包 -> 关 -> 系统 -> 小豆 -> 豆包
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 6);
    assert(flow.settings.countdown_voice == 3);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 4);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 5);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 6);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 7);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 0);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 1);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 2);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.countdown_voice == 3);

    // 语音、滚动录入、音量、Wi-Fi
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 7);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.voice_enabled == 0);

    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 8);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.roll_entry == 1);

    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 9);
    assert(flow.settings.beep_volume == 3);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.beep_volume == 4);

    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.settings_row == 10);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SETTINGS_CHANGED);
    assert(flow.settings.wifi_save == 0);

    // 长按确定保存并回主页
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK_LONG) == ARCHERY_ACT_SETTINGS_SAVE);
    assert(flow.page == ARCHERY_PAGE_HOME);
}

static void test_history_pages(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HISTORY);
    assert(flow.history_cursor == 0);
    assert(flow.history_filter == ARCHERY_HISTORY_FILTER_ALL);

    // 空历史：只显示一行"返回主页"，确定即退出
    archery_flow_set_history_count(&flow, 0);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.history_cursor == 0);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HOME);

    // 重新进入，3 条记录：行 0..2 是记录，3 清空，4 返回；首尾循环
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HISTORY);
    archery_flow_set_history_count(&flow, 3);
    press(&flow, ARCHERY_KEY_UP);  // 0 -> 最后一行
    assert(flow.history_cursor == 4);
    press(&flow, ARCHERY_KEY_DOWN);  // 最后一行 -> 0
    assert(flow.history_cursor == 0);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.history_cursor == 4);
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.history_cursor == 3);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_CLEAR_CONFIRM);
    press(&flow, ARCHERY_KEY_OK);  // 取消
    assert(flow.page == ARCHERY_PAGE_HISTORY);

    // 详情
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.history_cursor == 2);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_DETAIL);
    assert(flow.detail_group == 0);
    flow.detail_groups = 3;
    for (int i = 0; i < 5; i++) press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.detail_group == 2);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HISTORY);

    // 双击确定循环切换筛选：全部 -> 今天 -> 最近 7 天 -> 全部
    assert(archery_flow_history_cycle_filter(&flow) ==
           ARCHERY_ACT_HISTORY_FILTER);
    assert(flow.history_filter == ARCHERY_HISTORY_FILTER_TODAY);
    assert(flow.history_cursor == 0);
    assert(archery_flow_history_cycle_filter(&flow) ==
           ARCHERY_ACT_HISTORY_FILTER);
    assert(flow.history_filter == ARCHERY_HISTORY_FILTER_WEEK);
    assert(archery_flow_history_cycle_filter(&flow) ==
           ARCHERY_ACT_HISTORY_FILTER);
    assert(flow.history_filter == ARCHERY_HISTORY_FILTER_ALL);

    // 长按确定 = 直接回主页
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK_LONG) == ARCHERY_ACT_NONE);
    assert(flow.page == ARCHERY_PAGE_HOME);

    // 再次进入历史，清空
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HISTORY);
    for (int i = 0; i < 3; i++) press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.history_cursor == 3);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_CLEAR_CONFIRM);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.confirm_yes);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_CLEAR_HISTORY);
    assert(flow.page == ARCHERY_PAGE_HISTORY);
    assert(flow.history_count == 0);

    // 取消清空
    archery_flow_set_history_count(&flow, 1);
    press(&flow, ARCHERY_KEY_DOWN);  // 0 -> 1（清空行）
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_CLEAR_CONFIRM);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_NONE);
    assert(flow.page == ARCHERY_PAGE_HISTORY);
    assert(flow.history_count == 1);

    // 返回行回主页
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.history_cursor == 2);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_HOME);
}


static void test_wifi_page(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    // 主页五项：下移到「联网」并进入
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.home_select == 5);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_ENTER);
    assert(flow.page == ARCHERY_PAGE_WIFI);
    assert(flow.wifi_mode == 0 && flow.wifi_row == 0);

    // 扫描：进入列表
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_SCAN);
    assert(flow.wifi_mode == 1 && flow.wifi_row == 0);
    archery_flow_set_wifi_scan(&flow, 3);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.wifi_row == 1);
    // 选网先进入密码页
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_NONE);
    assert(flow.page == ARCHERY_PAGE_WIFI_PASSWORD);
    assert(flow.wifi_target == 1);
    // 首位长按返回扫描
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK_LONG) == ARCHERY_ACT_WIFI_SCAN);
    assert(flow.page == ARCHERY_PAGE_WIFI && flow.wifi_mode == 1);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.wifi_row == 3);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_NONE);
    assert(flow.wifi_mode == 0);

    // 菜单：手机扫码配网 / 立即对时 / 断开
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.wifi_row == 1);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_PROVISION);
    // 配网热点已开时再按"确定"= 停止配网并留在联网菜单（UI 提示"确定 停止配网"）。
    // 回归：此前这里无条件返回 WIFI_PROVISION，start 在已运行时直接返回 ESP_OK，
    // 表现为短按确定毫无反应，只能长按回主页。
    archery_flow_set_wifi_provisioning(&flow, true);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_STOP);
    assert(flow.page == ARCHERY_PAGE_WIFI);
    assert(flow.wifi_mode == 0 && flow.wifi_row == 1);
    archery_flow_set_wifi_provisioning(&flow, false);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_PROVISION);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.wifi_row == 2);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_SYNC_TIME);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.wifi_row == 3);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_WIFI_STOP);

    // 第 5 行是验证语音，不再直接离开
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.wifi_row == 4);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_VOICE_VERIFY);
    assert(flow.page == ARCHERY_PAGE_WIFI);

    // 长按确定离开
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK_LONG) == ARCHERY_ACT_WIFI_LEAVE);
    assert(flow.page == ARCHERY_PAGE_HOME);

    // 扫描结果清空后光标钳制
    archery_flow_set_wifi_scan(&flow, 0);
    assert(flow.wifi_ap_count == 0);
}


static void test_wifi_password(void) {
    // 字符表：0-9、A-Z、a-z，首尾回绕
    assert(archery_wifi_password_char(0) == '0');
    assert(archery_wifi_password_char(9) == '9');
    assert(archery_wifi_password_char(10) == 'A');
    assert(archery_wifi_password_char(35) == 'Z');
    assert(archery_wifi_password_char(36) == 'a');
    assert(archery_wifi_password_char(61) == 'z');
    assert(archery_wifi_password_char(62) == '0');
    assert(archery_wifi_password_next('0', -1) == 61);
    assert(archery_wifi_password_next('z', 1) == 0);

    archery_flow_t flow;
    archery_flow_init(&flow);
    flow.page = ARCHERY_PAGE_WIFI;
    flow.wifi_mode = 1;
    flow.wifi_ap_count = 2;
    archery_flow_set_wifi_security(&flow, 0x02);  // 第二个网络加密
    flow.wifi_row = 1;
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_NONE);
    assert(flow.page == ARCHERY_PAGE_WIFI_PASSWORD);
    assert(flow.wifi_target == 1);
    assert(flow.wifi_pw_len == 0 && flow.wifi_pw_pos == 0);

    // 空位按上：出现 '1'（从 '0' 前进一位）
    press(&flow, ARCHERY_KEY_UP);
    assert(flow.wifi_pw_len == 1);
    assert(flow.wifi_password[0] == '1');
    // 确定下一位
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.wifi_pw_pos == 1);
    // 短按确定只移动光标，不再自动连接（连接由双击确定触发）
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_NONE);
    assert(flow.wifi_pw_pos == 2);
    assert(strcmp(flow.wifi_password, "1") == 0);

    // 长按删除：两位退到一位，再退到空
    flow.page = ARCHERY_PAGE_WIFI_PASSWORD;
    flow.wifi_pw_pos = 2;
    flow.wifi_pw_len = 2;
    flow.wifi_password[0] = 'a';
    flow.wifi_password[1] = 'b';
    flow.wifi_password[2] = '\0';
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.wifi_pw_pos == 1 && flow.wifi_pw_len == 1);
    assert(strcmp(flow.wifi_password, "a") == 0);
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.wifi_pw_pos == 0 && flow.wifi_pw_len == 0);
    // 首位长按返回列表
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_WIFI);
    assert(flow.wifi_mode == 1);
}

static void test_timer_flow(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    flow.settings.timer_seconds = 20;
    flow.page = ARCHERY_PAGE_SETUP;
    flow.setup_row = 2;  // 单人靶的"开始"行
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_TIMER_START);
    assert(flow.page == ARCHERY_PAGE_TIMER);
    assert(flow.timer_phase == 0);
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_TIMER_STOP);
    assert(flow.page == ARCHERY_PAGE_ENTRY);

    archery_flow_init(&flow);
    flow.settings.timer_seconds = 0;
    flow.page = ARCHERY_PAGE_SETUP;
    flow.setup_row = 2;
    assert(archery_flow_handle(&flow, ARCHERY_KEY_OK) == ARCHERY_ACT_SESSION_START);
    assert(flow.page == ARCHERY_PAGE_ENTRY);
}

static void test_diag_pages(void) {
    archery_flow_t flow;
    archery_flow_init(&flow);
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_DIAG);
    assert(flow.diag_row == 0);
    press(&flow, ARCHERY_KEY_DOWN);
    press(&flow, ARCHERY_KEY_DOWN);
    assert(flow.diag_row == 2);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_DIAG_LOG);
    press(&flow, ARCHERY_KEY_OK);
    assert(flow.page == ARCHERY_PAGE_DIAG);
    press(&flow, ARCHERY_KEY_OK_LONG);
    assert(flow.page == ARCHERY_PAGE_HOME);
}

int main(void) {
    test_home_and_setup();
    test_ring_count_entry();
    test_incomplete_group_cannot_commit();
    test_full_tally_shortcut();
    test_abort_paths();
    test_button_role_rotation();
    test_full_target_entry();
    test_entry_without_x();
    test_triple_target_setup_and_entry();
    test_settings_page();
    test_history_pages();
    test_wifi_page();
    test_wifi_password();
    test_timer_flow();
    test_diag_pages();
    puts("archery flow state-machine tests: PASS");
    return 0;
}
