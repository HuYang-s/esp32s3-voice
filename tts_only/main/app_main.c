#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "opus.h"

// Speaker I2S pins
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

// Mic pins (not used in this minimal speaker test, but kept for completeness)
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// Extra GPIOs (not used here)
#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39
#define RESET_NVS_BUTTON_GPIO     GPIO_NUM_1
#define RESET_FACTORY_BUTTON_GPIO GPIO_NUM_2

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define I2S_SAMPLE_RATE_HZ   16000
#define I2S_BITS_PER_SAMPLE  I2S_DATA_BIT_WIDTH_32BIT
#define I2S_SLOT_MODE        I2S_SLOT_MODE_MONO

static const char *TAG = "TTS_ONLY";
// Expose TX channel for websocket player
i2s_chan_handle_t i2s_tx_chan = NULL;
// Move audio buffers to static storage to reduce stack usage of main task
static int16_t g_pcm_mono[960];                 // 60ms @ 16kHz
static int32_t g_i2s_buffer32[960];             // 60ms mono @16kHz, 32-bit samples

extern const uint8_t err_reg_p3_start[] asm("_binary_err_reg_p3_start");
extern const uint8_t err_reg_p3_end[]   asm("_binary_err_reg_p3_end");
extern const uint8_t err_pin_p3_start[] asm("_binary_err_pin_p3_start");
extern const uint8_t err_pin_p3_end[]   asm("_binary_err_pin_p3_end");
extern const uint8_t err_wificonfig_p3_start[] asm("_binary_err_wificonfig_p3_start");
extern const uint8_t err_wificonfig_p3_end[]   asm("_binary_err_wificonfig_p3_end");

static void init_i2s_tx(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = I2S_SAMPLE_RATE_HZ,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_SPK_GPIO_BCLK,
            .ws   = AUDIO_I2S_SPK_GPIO_LRCK,
            .dout = AUDIO_I2S_SPK_GPIO_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
}

// Simple P3 packet structure: [1B type][1B reserved][2B big-endian payload_size][opus_data]
static void play_p3_asset(const uint8_t* start, size_t size)
{
    int opus_sample_rate = 16000; // assets encoded at 16kHz mono, 60ms frames
    OpusDecoder* decoder = opus_decoder_create(opus_sample_rate, 1, NULL);
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to create Opus decoder");
        return;
    }

    const uint8_t* p = start;
    const uint8_t* end = start + size;

    // Decode loop
    while (p + 4 <= end) {
        uint8_t type = p[0];
        (void)type; // currently unused
        uint16_t payload_size = ((uint16_t)p[2] << 8) | (uint16_t)p[3];
        p += 4;
        if (p + payload_size > end) {
            break;
        }
        const unsigned char* opus = (const unsigned char*)p;
        p += payload_size;

        // 60ms at 16kHz mono -> 960 samples
        int frame_size = opus_sample_rate * 60 / 1000;
        int16_t* pcm_mono = g_pcm_mono;
        int decoded = opus_decode(decoder, opus, payload_size, pcm_mono, frame_size, 0);
        if (decoded < 0) {
            ESP_LOGW(TAG, "Opus decode error: %d", decoded);
            continue;
        }
        size_t frames = (size_t)decoded;

        // Convert 16-bit PCM to 32-bit samples (left-justified via <<16)
        for (size_t i = 0; i < frames; ++i) {
            g_i2s_buffer32[i] = ((int32_t)pcm_mono[i]) << 16;
        }
        size_t bytes_written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(i2s_tx_chan, g_i2s_buffer32, frames * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    }

    opus_decoder_destroy(decoder);
}

void ws_tts_run(void);

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "TTS-only project (ESP-IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip features: %d cores, WiFi%s%s, silicon rev %d",
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "",
             chip_info.revision);

    // Initialize I2S TX for speaker output
    init_i2s_tx();
    ESP_LOGI(TAG, "I2S TX initialized: BCLK=%d, LRCK=%d, DOUT=%d, fs=%d Hz, 32-bit mono-left",
             AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_SAMPLE_RATE_HZ);

    // Self-test: play one embedded P3 clip once to verify audio path
    size_t test_size = (size_t)(err_wificonfig_p3_end - err_wificonfig_p3_start);
    ESP_LOGI(TAG, "Self-test: playing embedded err_wificonfig.p3 (%u bytes)", (unsigned)test_size);
    play_p3_asset(err_wificonfig_p3_start, test_size);

    // Start websocket TTS streaming (ASR/LLM on server side)
    ws_tts_run();
}