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
#define TONE_FREQUENCY_HZ    1000
#define TONE_AMPLITUDE       5000
#define SAMPLES_PER_WRITE    256

static const char *TAG = "TTS_ONLY";
static i2s_chan_handle_t i2s_tx_chan = NULL;

static void init_i2s_tx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Moderate DMA sizes to reduce latency and keep the clock running
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

    ESP_ERROR_CHECK(i2s_channel_init_std(i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
}

static void generate_tone_block(int16_t *out_interleaved_lr, size_t frames, float *phase)
{
    const float phase_inc = 2.0f * (float)M_PI * (float)TONE_FREQUENCY_HZ / (float)I2S_SAMPLE_RATE_HZ;
    for (size_t i = 0; i < frames; ++i) {
        int16_t s = (int16_t)((float)TONE_AMPLITUDE * sinf(*phase));
        *phase += phase_inc;
        if (*phase > 2.0f * (float)M_PI) {
            *phase -= 2.0f * (float)M_PI;
        }
        // Interleave L/R
        out_interleaved_lr[2 * i + 0] = s;
        out_interleaved_lr[2 * i + 1] = s;
    }
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

    // Generate and stream a continuous 1 kHz sine tone as a hardware smoke test
    int16_t pcm[SAMPLES_PER_WRITE * 2]; // stereo interleaved
    float phase = 0.0f;

    while (true) {
        generate_tone_block(pcm, SAMPLES_PER_WRITE, &phase);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(i2s_tx_chan, pcm, sizeof(pcm), &bytes_written, 1000);
        if (err != ESP_OK || bytes_written != sizeof(pcm)) {
            ESP_LOGW(TAG, "i2s write err=%d bytes=%u", (int)err, (unsigned)bytes_written);
        }
        // Optional: log heartbeat at ~1s cadence without disrupting audio
        static int counter = 0;
        if (++counter >= (I2S_SAMPLE_RATE_HZ / SAMPLES_PER_WRITE)) {
            ESP_LOGI(TAG, "Streaming tone...");
            counter = 0;
        }
    }
}