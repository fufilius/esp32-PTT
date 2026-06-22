#include "network_audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "status_leds.h"

static const char *TAG = "network_audio";

#define NETWORK_AUDIO_MAGIC 0x41554431u
#define NETWORK_AUDIO_VERSION 1
#define NETWORK_AUDIO_QUEUE_LEN 32
#define NETWORK_AUDIO_SAMPLES_PER_PACKET 96
#define NETWORK_AUDIO_DEFAULT_RECORD_MS 3000

#define NETWORK_AUDIO_FLAG_START 0x01
#define NETWORK_AUDIO_FLAG_END 0x02

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t sequence;
    uint8_t sample_count;
    uint8_t reserved[3];
    int16_t samples[NETWORK_AUDIO_SAMPLES_PER_PACKET];
} network_audio_packet_t;

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static QueueHandle_t s_rx_queue;
static bool s_streaming_tx;
static bool s_streaming_tx_first_packet;
static uint16_t s_streaming_tx_sequence;

static esp_err_t network_audio_send_packet(const int16_t *samples, uint8_t sample_count, uint8_t flags, uint16_t sequence)
{
    network_audio_packet_t packet = {
        .magic = NETWORK_AUDIO_MAGIC,
        .version = NETWORK_AUDIO_VERSION,
        .flags = flags,
        .sequence = sequence,
        .sample_count = sample_count,
    };

    if (sample_count > 0) {
        memcpy(packet.samples, samples, sample_count * sizeof(packet.samples[0]));
    }

    return esp_now_send(s_broadcast_mac, (const uint8_t *)&packet, sizeof(packet));
}

static void network_audio_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    (void)recv_info;

    if (data == NULL || data_len != sizeof(network_audio_packet_t) || s_rx_queue == NULL) {
        return;
    }

    network_audio_packet_t packet;
    memcpy(&packet, data, sizeof(packet));
    if (packet.magic != NETWORK_AUDIO_MAGIC || packet.version != NETWORK_AUDIO_VERSION ||
        packet.sample_count > NETWORK_AUDIO_SAMPLES_PER_PACKET) {
        return;
    }

    (void)xQueueSend(s_rx_queue, &packet, 0);
}

static void network_audio_rx_task(void *arg)
{
    (void)arg;

    bool speaker_started = false;
    network_audio_packet_t packet;

    while (true) {
        if (xQueueReceive(s_rx_queue, &packet, pdMS_TO_TICKS(250)) != pdTRUE) {
            if (speaker_started) {
                audio_speaker_stop();
                speaker_started = false;
                status_leds_set_rx(false);
                ESP_LOGW(TAG, "receive timeout, speaker stopped");
            }
            continue;
        }

        if ((packet.flags & NETWORK_AUDIO_FLAG_START) != 0) {
            if (speaker_started) {
                audio_speaker_stop();
            }
            ESP_ERROR_CHECK(audio_speaker_start());
            speaker_started = true;
            status_leds_set_rx(true);
            ESP_LOGI(TAG, "incoming voice started");
        } else if (!speaker_started) {
            ESP_ERROR_CHECK(audio_speaker_start());
            speaker_started = true;
            status_leds_set_rx(true);
        }

        if (packet.sample_count > 0) {
            ESP_ERROR_CHECK(audio_speaker_write(packet.samples, packet.sample_count));
        }

        if ((packet.flags & NETWORK_AUDIO_FLAG_END) != 0) {
            ESP_ERROR_CHECK(audio_speaker_stop());
            speaker_started = false;
            status_leds_set_rx(false);
            ESP_LOGI(TAG, "incoming voice finished");
        }
    }
}

static esp_err_t network_audio_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t network_audio_init_wifi(void)
{
    ESP_RETURN_ON_ERROR(network_audio_init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");

    esp_err_t event_loop_err = esp_event_loop_create_default();
    if (event_loop_err != ESP_OK && event_loop_err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(event_loop_err, TAG, "event loop init failed");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE), TAG, "wifi channel failed");

    return ESP_OK;
}

esp_err_t network_audio_init(void)
{
    s_rx_queue = xQueueCreate(NETWORK_AUDIO_QUEUE_LEN, sizeof(network_audio_packet_t));
    ESP_RETURN_ON_FALSE(s_rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "create rx queue failed");

    ESP_RETURN_ON_ERROR(network_audio_init_wifi(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp-now init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(network_audio_recv_cb), TAG, "register recv cb failed");

    esp_now_peer_info_t peer = {
        .channel = 1,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, s_broadcast_mac, sizeof(s_broadcast_mac));
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add broadcast peer failed");

    BaseType_t task_ok = xTaskCreate(network_audio_rx_task, "network_audio_rx", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create rx task failed");

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG,
             "ESP-NOW ready on channel 1, STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);

    return ESP_OK;
}

esp_err_t network_audio_send_recording(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        duration_ms = NETWORK_AUDIO_DEFAULT_RECORD_MS;
    }

    const size_t sample_count = ((size_t)CONFIG_IPPHONE_I2S_SAMPLE_RATE * duration_ms) / 1000;
    int16_t *recorded = (int16_t *)calloc(sample_count, sizeof(recorded[0]));
    ESP_RETURN_ON_FALSE(recorded != NULL, ESP_ERR_NO_MEM, TAG, "record buffer allocation failed");

    int32_t peak = 0;
    ESP_LOGI(TAG, "recording %lu ms for ESP-NOW", (unsigned long)duration_ms);
    status_leds_set_tx(true);
    esp_err_t err = audio_record_samples(recorded, sample_count, &peak);
    if (err != ESP_OK) {
        status_leds_set_tx(false);
        free(recorded);
        ESP_RETURN_ON_ERROR(err, TAG, "record for ESP-NOW failed");
    }

    ESP_LOGI(TAG, "sending voice, samples=%u peak=%ld", (unsigned)sample_count, peak);

    uint16_t sequence = 0;
    for (size_t offset = 0; offset < sample_count; offset += NETWORK_AUDIO_SAMPLES_PER_PACKET) {
        const size_t remaining = sample_count - offset;
        const uint8_t packet_samples =
            (uint8_t)(remaining < NETWORK_AUDIO_SAMPLES_PER_PACKET ? remaining : NETWORK_AUDIO_SAMPLES_PER_PACKET);

        uint8_t flags = 0;

        if (offset == 0) {
            flags |= NETWORK_AUDIO_FLAG_START;
        }
        if (offset + packet_samples >= sample_count) {
            flags |= NETWORK_AUDIO_FLAG_END;
        }

        err = network_audio_send_packet(&recorded[offset], packet_samples, flags, sequence++);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp-now send failed: %s", esp_err_to_name(err));
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(6));
    }

    free(recorded);
    status_leds_set_tx(false);
    ESP_RETURN_ON_ERROR(err, TAG, "send voice failed");
    ESP_LOGI(TAG, "voice sent");
    return ESP_OK;
}

esp_err_t network_audio_stream_start(void)
{
    ESP_RETURN_ON_FALSE(!s_streaming_tx, ESP_ERR_INVALID_STATE, TAG, "stream tx is already running");

    esp_err_t err = audio_mic_start();
    ESP_RETURN_ON_ERROR(err, TAG, "start live mic failed");

    s_streaming_tx = true;
    s_streaming_tx_first_packet = true;
    s_streaming_tx_sequence = 0;
    status_leds_set_tx(true);
    ESP_LOGI(TAG, "live voice stream started");
    return ESP_OK;
}

esp_err_t network_audio_stream_send_next(bool end_after_packet)
{
    ESP_RETURN_ON_FALSE(s_streaming_tx, ESP_ERR_INVALID_STATE, TAG, "stream tx is not running");

    int16_t samples[NETWORK_AUDIO_SAMPLES_PER_PACKET] = {0};
    size_t samples_read = 0;
    int32_t peak = 0;
    esp_err_t err = audio_mic_read(samples, NETWORK_AUDIO_SAMPLES_PER_PACKET, &samples_read, &peak);
    ESP_RETURN_ON_ERROR(err, TAG, "read live mic failed");

    uint8_t flags = 0;
    if (s_streaming_tx_first_packet) {
        flags |= NETWORK_AUDIO_FLAG_START;
        s_streaming_tx_first_packet = false;
    }
    if (end_after_packet) {
        flags |= NETWORK_AUDIO_FLAG_END;
    }

    err = network_audio_send_packet(samples, (uint8_t)samples_read, flags, s_streaming_tx_sequence++);
    ESP_RETURN_ON_ERROR(err, TAG, "send live packet failed");

    if (end_after_packet) {
        ESP_RETURN_ON_ERROR(audio_mic_stop(), TAG, "stop live mic failed");
        s_streaming_tx = false;
        status_leds_set_tx(false);
        ESP_LOGI(TAG, "live voice stream stopped, last peak=%ld", peak);
    }

    return ESP_OK;
}

esp_err_t network_audio_stream_stop(void)
{
    if (!s_streaming_tx) {
        return ESP_OK;
    }

    int16_t zero_sample = 0;
    uint8_t flags = NETWORK_AUDIO_FLAG_END;
    if (s_streaming_tx_first_packet) {
        flags |= NETWORK_AUDIO_FLAG_START;
    }

    esp_err_t send_err = network_audio_send_packet(&zero_sample, 1, flags, s_streaming_tx_sequence++);
    esp_err_t mic_err = audio_mic_stop();
    s_streaming_tx = false;
    s_streaming_tx_first_packet = false;
    status_leds_set_tx(false);

    ESP_RETURN_ON_ERROR(mic_err, TAG, "stop live mic failed");
    ESP_RETURN_ON_ERROR(send_err, TAG, "send live end packet failed");
    ESP_LOGI(TAG, "live voice stream stopped");
    return ESP_OK;
}
