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

#define I2S_SAMPLE_RATE_HZ   48000
#define I2S_BITS_PER_SAMPLE  I2S_DATA_BIT_WIDTH_16BIT
#define I2S_SLOT_MODE        I2S_SLOT_MODE_STEREO

static const char *TAG = "TTS_ONLY";
static i2s_chan_handle_t i2s_tx_chan = NULL;
// Move audio buffers to static storage to reduce stack usage of main task
static int16_t g_pcm_mono[960];                 // 60ms @ 16kHz
static int16_t g_i2s_buffer[320 * 3 * 2];       // chunk 320 -> upsample x3 -> stereo

extern const uint8_t err_reg_p3_start[] asm("_binary_err_reg_p3_start");
extern const uint8_t err_reg_p3_end[]   asm("_binary_err_reg_p3_end");
extern const uint8_t err_pin_p3_start[] asm("_binary_err_pin_p3_start");
extern const uint8_t err_pin_p3_end[]   asm("_binary_err_pin_p3_end");
extern const uint8_t err_wificonfig_p3_start[] asm("_binary_err_wificonfig_p3_start");
extern const uint8_t err_wificonfig_p3_end[]   asm("_binary_err_wificonfig_p3_end");

static void init_i2s_tx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL));

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE_HZ);

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BITS_PER_SAMPLE, I2S_SLOT_MODE);
    // Ensure slot bit width equals data width for common amps like MAX98357A
    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    i2s_std_gpio_config_t gpio_cfg = {
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
    };

    i2s_std_config_t std_cfg = {
        .clk_cfg  = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = gpio_cfg,
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

        // Up-sample from 16k -> 48k by simple duplication (nearest). For quality, use proper resampler if needed.
        // 3x upsample and duplicate to stereo
        for (size_t blk = 0; blk < frames; ) {
            size_t chunk = frames - blk;
            if (chunk > 320) chunk = 320; // limit temporary buffer
            int16_t* i2s_buffer = g_i2s_buffer; // mono 320 -> 960, stereo -> *2
            size_t out_idx = 0;
            for (size_t i = 0; i < chunk; ++i) {
                int16_t s = pcm_mono[blk + i];
                // 3x duplicate for 48k
                for (int k = 0; k < 3; ++k) {
                    i2s_buffer[out_idx++] = s; // L
                    i2s_buffer[out_idx++] = s; // R
                }
            }
            size_t bytes_written = 0;
            ESP_ERROR_CHECK(i2s_channel_write(i2s_tx_chan, i2s_buffer, out_idx * sizeof(int16_t), &bytes_written, portMAX_DELAY));
        
            blk += chunk;
        }
    }

    opus_decoder_destroy(decoder);
}

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
    ESP_LOGI(TAG, "I2S TX initialized: BCLK=%d, LRCK=%d, DOUT=%d, fs=%d Hz, 16-bit stereo",
             AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_SAMPLE_RATE_HZ);

    // Play embedded P3 assets in a loop as a TTS demo
    while (true) {
        ESP_LOGI(TAG, "Playing err_reg.p3...");
        play_p3_asset(err_reg_p3_start, err_reg_p3_end - err_reg_p3_start);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Playing err_pin.p3...");
        play_p3_asset(err_pin_p3_start, err_pin_p3_end - err_pin_p3_start);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Playing err_wificonfig.p3...");
        play_p3_asset(err_wificonfig_p3_start, err_wificonfig_p3_end - err_wificonfig_p3_start);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}