#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <vector>
#include <string>
#include <arpa/inet.h>
#include <opus.h>

#include "audio_codecs/no_audio_codec.h"

#define TAG "tts_only"

// I2S pins and rates: copy from bread-compact-wifi config
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

// Protocol v3 header for embedded opus packets
struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size; // big endian on wire
    uint8_t payload[];
} __attribute__((packed));

extern const char err_reg_p3_start[] asm("_binary_err_reg_p3_start");
extern const char err_reg_p3_end[]   asm("_binary_err_reg_p3_end");

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for future settings if needed
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize I2S codec (speaker + mic simplex, but we will only output)
    static NoAudioCodec codec(
        AUDIO_INPUT_SAMPLE_RATE,
        AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_SPK_GPIO_BCLK,
        AUDIO_I2S_SPK_GPIO_LRCK,
        AUDIO_I2S_SPK_GPIO_DOUT,
        AUDIO_I2S_MIC_GPIO_SCK,
        AUDIO_I2S_MIC_GPIO_WS,
        AUDIO_I2S_MIC_GPIO_DIN
    );
    codec.SetOutputVolume(80);
    codec.Start();

    // Create Opus decoder at output sample rate (we will resample by Opus if needed)
    OpusDecoder* opus_decoder = opus_decoder_create(AUDIO_OUTPUT_SAMPLE_RATE, 1, NULL);

    // Decode embedded P3 stream and push PCM to codec
    const char* data = err_reg_p3_start;
    const size_t size = err_reg_p3_end - err_reg_p3_start;
    for (const char* p = data; p < data + size; ) {
        auto p3 = (const BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        const unsigned char* opus = (const unsigned char*)p3->payload;
        p += payload_size;

        int frame_size = AUDIO_OUTPUT_SAMPLE_RATE * 60 / 1000; // 60ms
        std::vector<int16_t> pcm(frame_size);
        int decoded = opus_decode(opus_decoder, opus, payload_size, pcm.data(), frame_size, 0);
        if (decoded < 0) {
            ESP_LOGE(TAG, "Opus decode error: %d", decoded);
            continue;
        }
        pcm.resize(decoded);
        codec.OutputData(pcm);
    }
    // Wait until all audio frames are sent
    codec.WaitForOutputDone();

    // Idle loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}