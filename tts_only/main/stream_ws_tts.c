#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_websocket_client.h>
#include <cJSON.h>
#include <opus.h>
#include "driver/i2s_std.h"

static const char* TAG = "WS_TTS";

// External I2S TX handle from app_main.c
extern i2s_chan_handle_t i2s_tx_chan;

#ifndef CONFIG_WEBSOCKET_URL
#define CONFIG_WEBSOCKET_URL "wss://example.com/ws"
#endif
#ifndef CONFIG_EXAMPLE_WIFI_SSID
#define CONFIG_EXAMPLE_WIFI_SSID "ssid"
#endif
#ifndef CONFIG_EXAMPLE_WIFI_PASSWORD
#define CONFIG_EXAMPLE_WIFI_PASSWORD "password"
#endif
#ifndef CONFIG_WEBSOCKET_ACCESS_TOKEN
#define CONFIG_WEBSOCKET_ACCESS_TOKEN ""
#endif

static int g_server_sample_rate = 16000;

// Minimal Opus decode and write to I2S (32-bit mono-left)
static void decode_and_play_opus_packet(const uint8_t* data, size_t len, OpusDecoder* decoder) {
    if (len == 0) return;
    int frame_size = g_server_sample_rate * 60 / 1000; // 60ms
    static int16_t pcm_mono[1920]; // enough for up to 32kHz@60ms
    int ret = opus_decode(decoder, data, len, pcm_mono, frame_size, 0);
    if (ret <= 0) {
        ESP_LOGW(TAG, "opus_decode ret=%d", ret);
        return;
    }
    static int32_t buffer32[1920];
    for (int i = 0; i < ret; ++i) buffer32[i] = ((int32_t)pcm_mono[i]) << 16;
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(i2s_tx_chan, buffer32, ret * sizeof(int32_t), &bytes_written, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s write err=%d bw=%u", err, (unsigned)bytes_written);
    }
}

typedef struct {
    esp_websocket_client_handle_t client;
    OpusDecoder* decoder;
} ws_ctx_t;

static void parse_server_hello(const cJSON* root, ws_ctx_t* ctx) {
    const cJSON* transport = cJSON_GetObjectItem(root, "transport");
    if (!transport || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGW(TAG, "Unsupported transport");
        return;
    }
    const cJSON* audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (audio_params) {
        const cJSON* sr = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (sr && sr->valueint > 0) {
            g_server_sample_rate = sr->valueint;
            opus_decoder_destroy(ctx->decoder);
            int err = 0;
            ctx->decoder = opus_decoder_create(g_server_sample_rate, 1, &err);
            ESP_LOGI(TAG, "Server hello: sample_rate=%d (decoder recreated, err=%d)", g_server_sample_rate, err);
        }
    }
}

static void on_ws_event(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    ws_ctx_t* ctx = (ws_ctx_t*)handler_args;
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WS connected");
        // Build hello JSON aligned to main project
        char hello[192];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}" );
        esp_websocket_client_send_text(ctx->client, hello, strlen(hello), portMAX_DELAY);
        ESP_LOGI(TAG, "hello sent");
        break; }
    case WEBSOCKET_EVENT_DATA: {
        if (data->op_code == 0x1) { // text
            ESP_LOGD(TAG, "text %.*s", data->data_len, (const char*)data->data_ptr);
            cJSON* root = cJSON_ParseWithLength((const char*)data->data_ptr, data->data_len);
            if (root) {
                const cJSON* type = cJSON_GetObjectItem(root, "type");
                if (type && strcmp(type->valuestring, "hello") == 0) {
                    parse_server_hello(root, ctx);
                } else if (type && strcmp(type->valuestring, "tts") == 0) {
                    const cJSON* state = cJSON_GetObjectItem(root, "state");
                    if (state && strcmp(state->valuestring, "start") == 0) {
                        opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
                        ESP_LOGI(TAG, "tts start");
                    } else if (state && strcmp(state->valuestring, "stop") == 0) {
                        ESP_LOGI(TAG, "tts stop");
                    }
                }
                cJSON_Delete(root);
            }
        } else if (data->op_code == 0x2) { // binary
            ESP_LOGI(TAG, "bin %d bytes", data->data_len);
            decode_and_play_opus_packet((const uint8_t*)data->data_ptr, data->data_len, ctx->decoder);
        }
        break; }
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error");
        break;
    default:
        break;
    }
}

// Very small WiFi helper (station)
static esp_err_t wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD, sizeof(wifi_config.sta.password)-1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi init sta started");

    ESP_ERROR_CHECK(esp_wifi_connect());
    return ESP_OK;
}

// Websocket streaming: send hello, receive binary opus packets and play
void ws_tts_run(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wifi_init_sta());

    esp_websocket_client_config_t ws_cfg = {
        .uri = CONFIG_WEBSOCKET_URL,
        .disable_auto_reconnect = false,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "websocket init failed");
        return;
    }

    if (strlen(CONFIG_WEBSOCKET_ACCESS_TOKEN) > 0) {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_WEBSOCKET_ACCESS_TOKEN);
        esp_websocket_client_append_header(client, "Authorization", auth);
    }
    esp_websocket_client_append_header(client, "Protocol-Version", "1");
    esp_websocket_client_append_header(client, "Device-Id", "esp32s3"); // simple id

    int err = 0;
    OpusDecoder* decoder = opus_decoder_create(g_server_sample_rate, 1, &err);
    if (!decoder || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus decoder create failed err=%d", err);
        esp_websocket_client_destroy(client);
        return;
    }

    ws_ctx_t ctx = { .client = client, .decoder = decoder };
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, on_ws_event, &ctx);

    if (!esp_websocket_client_start(client)) {
        ESP_LOGE(TAG, "websocket connect failed");
        esp_websocket_client_destroy(client);
        opus_decoder_destroy(decoder);
        return;
    }

    while (esp_websocket_client_is_connected(client)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    opus_decoder_destroy(decoder);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
}