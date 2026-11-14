set(ARCHERY_MUSIC_EMBED_FILES)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/assets/xixige_22k.pcm")
    list(APPEND ARCHERY_MUSIC_EMBED_FILES "assets/xixige_22k.pcm")
endif()

idf_component_register(
    SRCS "main.c"
         "archery_app.c"
         "archery_clock.c"
         "archery_flow.c"
         "archery_history.c"
         "archery_layout.c"
         "archery_logbuf.c"
         "archery_name.c"
         "archery_score.c"
         "archery_speech.c"
         "archery_session.c"
         "archery_settings.c"
         "archery_metro.c"
         "archery_sight.c"
         "archery_store.c"
         "archery_wifi.c"
         "archery_wifi_store.c"
         "archery_wifi_store_nvs.c"
         "archery_qr.c"
         "archery_ui.c"
         "archery_voice.c"
         "archery_ws_client.c"
         "archery_fonts.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    EMBED_FILES ${ARCHERY_MUSIC_EMBED_FILES}
    REQUIRES bsp bt esp_event esp_hw_support esp_http_server esp_netif esp_timer esp-tls esp_wifi lwip mbedtls nvs_flash
)

# 应用专用的中文子集字体（Noto Sans SC，OFL-1.1）。生成命令与字符清单见
# assets/README.md；重新生成后无需改动这里。
if(ARCHERY_MUSIC_EMBED_FILES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ARCHERY_MUSIC_EMBEDDED=1)
endif()

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/archery_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/archery_font_20.c"
)
