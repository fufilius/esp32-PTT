#include "storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

#define STORAGE_MOUNT_POINT "/sdcard"
#define STORAGE_RECORDING_PATH STORAGE_MOUNT_POINT "/record.wav"
#define STORAGE_RECORD_CHUNK_SAMPLES 256
#define WAV_HEADER_SIZE 44
#define STORAGE_RECORD_STOP_TIMEOUT_MS 2000
#define STORAGE_RECORD_PROGRESS_TIMEOUT_MS 3000
#define STORAGE_SD_MOUNT_RETRIES 5
#define STORAGE_SD_MOUNT_RETRY_DELAY_MS 500

typedef enum {
    RECORD_STATE_IDLE,
    RECORD_STATE_STARTING,
    RECORD_STATE_ACTIVE,
    RECORD_STATE_STOPPING,
    RECORD_STATE_FAILED,
} record_state_t;

typedef struct {
    char riff[4];
    uint32_t file_size_minus_8;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channel_count;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

static bool s_recording;
static bool s_mounted;
static record_state_t s_record_state = RECORD_STATE_IDLE;
static uint32_t s_recorded_bytes;
static TickType_t s_last_progress_tick;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_record_mutex;
static TaskHandle_t s_record_task;
static TaskHandle_t s_record_start_waiter;
static TaskHandle_t s_record_stop_waiter;
static bool s_record_stop_requested;
static bool s_record_start_cancel_requested;
static esp_err_t s_record_task_result;

static wav_header_t make_wav_header(uint32_t data_size)
{
    const uint16_t channel_count = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t sample_rate = CONFIG_IPPHONE_I2S_SAMPLE_RATE;
    const uint16_t block_align = (channel_count * bits_per_sample) / 8;

    wav_header_t header = {
        .riff = {'R', 'I', 'F', 'F'},
        .file_size_minus_8 = data_size + WAV_HEADER_SIZE - 8,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1,
        .channel_count = channel_count,
        .sample_rate = sample_rate,
        .byte_rate = sample_rate * block_align,
        .block_align = block_align,
        .bits_per_sample = bits_per_sample,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_size,
    };

    return header;
}

static esp_err_t write_wav_header(FILE *file, uint32_t data_size)
{
    wav_header_t header = make_wav_header(data_size);
    return fwrite(&header, sizeof(header), 1, file) == 1 ? ESP_OK : ESP_FAIL;
}

static esp_err_t rewrite_wav_header(FILE *file, uint32_t data_size)
{
    ESP_RETURN_ON_FALSE(fseek(file, 0, SEEK_SET) == 0, ESP_FAIL, TAG, "seek wav header failed");
    ESP_RETURN_ON_ERROR(write_wav_header(file, data_size), TAG, "write final wav header failed");
    ESP_RETURN_ON_FALSE(fflush(file) == 0, ESP_FAIL, TAG, "flush wav header failed");
    return ESP_OK;
}

static esp_err_t record_chunk_to_sd(FILE *file, uint32_t *recorded_bytes)
{
    int16_t samples[STORAGE_RECORD_CHUNK_SAMPLES] = {0};
    size_t samples_read = 0;
    int32_t peak = 0;
    esp_err_t err = audio_mic_read(samples, STORAGE_RECORD_CHUNK_SAMPLES, &samples_read, &peak);
    ESP_RETURN_ON_ERROR(err, TAG, "read mic for sd failed");

    const size_t written = fwrite(samples, sizeof(samples[0]), samples_read, file);
    ESP_RETURN_ON_FALSE(written == samples_read, ESP_FAIL, TAG, "write recording failed: errno=%d", errno);
    *recorded_bytes += (uint32_t)(written * sizeof(samples[0]));

    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        s_recorded_bytes = *recorded_bytes;
        s_last_progress_tick = xTaskGetTickCount();
        xSemaphoreGive(s_record_mutex);
    }

    return ESP_OK;
}

static void configure_sd_spi_pullups(void)
{
    gpio_set_pull_mode(CONFIG_IPPHONE_SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_IPPHONE_SD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_IPPHONE_SD_SCK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_IPPHONE_SD_CS_GPIO, GPIO_PULLUP_ONLY);
}

static void log_sd_wiring_hint(void)
{
    ESP_LOGE(TAG,
             "Check microSD wiring: VCC=5V for AMS1117 modules or 3V3 for bare sockets, GND=GND, "
             "SCK=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, CS=GPIO%d",
             CONFIG_IPPHONE_SD_SCK_GPIO,
             CONFIG_IPPHONE_SD_MOSI_GPIO,
             CONFIG_IPPHONE_SD_MISO_GPIO,
             CONFIG_IPPHONE_SD_CS_GPIO);
}

static bool record_stop_requested(void)
{
    bool requested = false;
    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        requested = s_record_stop_requested || s_record_start_cancel_requested;
        xSemaphoreGive(s_record_mutex);
    }
    return requested;
}

static void record_finish(esp_err_t result, uint32_t recorded_bytes)
{
    TaskHandle_t start_waiter = NULL;
    TaskHandle_t stop_waiter = NULL;

    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        s_record_task_result = result;
        s_recorded_bytes = recorded_bytes;
        s_recording = false;
        s_record_state = result == ESP_OK ? RECORD_STATE_IDLE : RECORD_STATE_FAILED;
        s_record_task = NULL;
        s_record_stop_requested = false;
        s_record_start_cancel_requested = false;
        start_waiter = s_record_start_waiter;
        stop_waiter = s_record_stop_waiter;
        s_record_start_waiter = NULL;
        s_record_stop_waiter = NULL;
        xSemaphoreGive(s_record_mutex);
    }

    if (start_waiter != NULL) {
        xTaskNotifyGive(start_waiter);
    }
    if (stop_waiter != NULL) {
        xTaskNotifyGive(stop_waiter);
    }
}

static void storage_record_task(void *arg)
{
    (void)arg;

    FILE *file = fopen(STORAGE_RECORDING_PATH, "wb+");
    if (file == NULL) {
        ESP_LOGE(TAG, "open recording failed: errno=%d", errno);
        record_finish(ESP_FAIL, 0);
        vTaskDelete(NULL);
    }

    esp_err_t err = write_wav_header(file, 0);
    if (err == ESP_OK) {
        err = audio_mic_start();
    }

    if (err != ESP_OK) {
        fclose(file);
        record_finish(err, 0);
        vTaskDelete(NULL);
    }

    if (record_stop_requested()) {
        audio_mic_stop();
        fclose(file);
        record_finish(ESP_ERR_INVALID_STATE, 0);
        vTaskDelete(NULL);
    }

    uint32_t recorded_bytes = 0;
    TaskHandle_t start_waiter = NULL;
    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_record_start_cancel_requested) {
            s_record_stop_requested = true;
        } else {
            s_recording = true;
            s_record_state = RECORD_STATE_ACTIVE;
            s_recorded_bytes = 0;
            s_last_progress_tick = xTaskGetTickCount();
            start_waiter = s_record_start_waiter;
            s_record_start_waiter = NULL;
        }
        xSemaphoreGive(s_record_mutex);
    }
    if (start_waiter != NULL) {
        xTaskNotifyGive(start_waiter);
    }

    while (!record_stop_requested()) {
        err = record_chunk_to_sd(file, &recorded_bytes);
        if (err != ESP_OK) {
            break;
        }
    }

    esp_err_t mic_err = audio_mic_stop();
    esp_err_t header_err = rewrite_wav_header(file, recorded_bytes);
    const int close_err = fclose(file);

    if (err == ESP_OK) {
        err = mic_err;
    }
    if (err == ESP_OK) {
        err = header_err;
    }
    if (err == ESP_OK && close_err != 0) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "recording saved, bytes=%lu", (unsigned long)recorded_bytes);
    } else {
        ESP_LOGE(TAG, "recording finished with error: %s", esp_err_to_name(err));
    }

    record_finish(err, recorded_bytes);
    vTaskDelete(NULL);
}

esp_err_t storage_init(void)
{
    if (s_record_mutex == NULL) {
        s_record_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_record_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create record mutex failed");
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = CONFIG_IPPHONE_SD_SPI_FREQ_KHZ;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_IPPHONE_SD_MOSI_GPIO,
        .miso_io_num = CONFIG_IPPHONE_SD_MISO_GPIO,
        .sclk_io_num = CONFIG_IPPHONE_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    configure_sd_spi_pullups();

    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    ESP_RETURN_ON_ERROR(err, TAG, "sd spi bus init failed");

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_IPPHONE_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG,
             "mounting microSD over SPI: SCK=%d MOSI=%d MISO=%d CS=%d freq=%d kHz",
             CONFIG_IPPHONE_SD_SCK_GPIO,
             CONFIG_IPPHONE_SD_MOSI_GPIO,
             CONFIG_IPPHONE_SD_MISO_GPIO,
             CONFIG_IPPHONE_SD_CS_GPIO,
             CONFIG_IPPHONE_SD_SPI_FREQ_KHZ);

    err = ESP_FAIL;
    for (int attempt = 1; attempt <= STORAGE_SD_MOUNT_RETRIES; ++attempt) {
        err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
        if (err == ESP_OK) {
            s_mounted = true;
            sdmmc_card_print_info(stdout, s_card);
            ESP_LOGI(TAG, "microSD mounted at %s", STORAGE_MOUNT_POINT);
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "mount microSD attempt %d/%d failed: %s",
                 attempt,
                 STORAGE_SD_MOUNT_RETRIES,
                 esp_err_to_name(err));
        if (attempt < STORAGE_SD_MOUNT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(STORAGE_SD_MOUNT_RETRY_DELAY_MS));
        }
    }

    spi_bus_free(host.slot);
    ESP_LOGE(TAG, "mount microSD failed after %d attempts: %s", STORAGE_SD_MOUNT_RETRIES, esp_err_to_name(err));
    log_sd_wiring_hint();
    return err;
}

bool storage_record_is_active(void)
{
    bool active = false;
    if (s_record_mutex != NULL && xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        active = s_record_state == RECORD_STATE_STARTING || s_record_state == RECORD_STATE_ACTIVE ||
                 s_record_state == RECORD_STATE_STOPPING;
        xSemaphoreGive(s_record_mutex);
    }
    return active;
}

esp_err_t storage_record_start(void)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "microSD is not mounted");
    ESP_RETURN_ON_FALSE(s_record_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "record mutex is not initialized");

    TaskHandle_t task = NULL;
    (void)ulTaskNotifyTake(pdTRUE, 0);
    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_record_state != RECORD_STATE_IDLE && s_record_state != RECORD_STATE_FAILED) {
            xSemaphoreGive(s_record_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        s_record_task_result = ESP_OK;
        s_recorded_bytes = 0;
        s_last_progress_tick = xTaskGetTickCount();
        s_record_stop_requested = false;
        s_record_start_cancel_requested = false;
        s_record_state = RECORD_STATE_STARTING;
        s_record_start_waiter = xTaskGetCurrentTaskHandle();
        xSemaphoreGive(s_record_mutex);
    }

    BaseType_t created = xTaskCreate(storage_record_task, "sd_record", 6144, NULL, 5, &task);
    if (created != pdPASS) {
        if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
            s_record_start_waiter = NULL;
            s_record_task_result = ESP_ERR_NO_MEM;
            s_record_state = RECORD_STATE_FAILED;
            xSemaphoreGive(s_record_mutex);
        }
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_record_state == RECORD_STATE_STARTING || s_record_state == RECORD_STATE_ACTIVE ||
            s_record_state == RECORD_STATE_STOPPING) {
            s_record_task = task;
        }
        xSemaphoreGive(s_record_mutex);
    }

    const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STORAGE_RECORD_STOP_TIMEOUT_MS));
    if (notified == 0) {
        if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
            s_record_start_waiter = NULL;
            s_record_start_cancel_requested = true;
            s_record_stop_requested = true;
            s_record_state = RECORD_STATE_STOPPING;
            xSemaphoreGive(s_record_mutex);
        }
        ESP_LOGE(TAG, "record task start timeout");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        result = s_record_task_result;
        if (result != ESP_OK) {
            s_record_state = RECORD_STATE_FAILED;
        }
        xSemaphoreGive(s_record_mutex);
    }
    ESP_RETURN_ON_ERROR(result, TAG, "start recording failed");

    ESP_LOGI(TAG, "recording to %s", STORAGE_RECORDING_PATH);
    return ESP_OK;
}

esp_err_t storage_record_next(void)
{
    esp_err_t result = ESP_OK;
    bool active = false;
    TickType_t last_progress = 0;

    if (s_record_mutex != NULL && xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        result = s_record_task_result;
        active = s_record_state == RECORD_STATE_STARTING || s_record_state == RECORD_STATE_ACTIVE ||
                 s_record_state == RECORD_STATE_STOPPING;
        last_progress = s_last_progress_tick;
        xSemaphoreGive(s_record_mutex);
    }

    ESP_RETURN_ON_ERROR(result, TAG, "record task failed");
    if (active && (xTaskGetTickCount() - last_progress) > pdMS_TO_TICKS(STORAGE_RECORD_PROGRESS_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "record task progress timeout");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t storage_record_stop(void)
{
    ESP_RETURN_ON_FALSE(s_record_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "record mutex is not initialized");

    bool active = false;
    (void)ulTaskNotifyTake(pdTRUE, 0);
    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        active = s_record_state == RECORD_STATE_STARTING || s_record_state == RECORD_STATE_ACTIVE ||
                 s_record_state == RECORD_STATE_STOPPING;
        if (active) {
            s_record_state = RECORD_STATE_STOPPING;
            s_record_stop_waiter = xTaskGetCurrentTaskHandle();
            s_record_stop_requested = true;
        }
        xSemaphoreGive(s_record_mutex);
    }

    if (!active) {
        return ESP_OK;
    }

    const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STORAGE_RECORD_STOP_TIMEOUT_MS));
    if (notified == 0) {
        if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
            s_record_stop_waiter = NULL;
            xSemaphoreGive(s_record_mutex);
        }
        ESP_LOGE(TAG, "record task stop timeout");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    uint32_t recorded_bytes = 0;
    if (xSemaphoreTake(s_record_mutex, portMAX_DELAY) == pdTRUE) {
        result = s_record_task_result;
        recorded_bytes = s_recorded_bytes;
        xSemaphoreGive(s_record_mutex);
    }
    ESP_RETURN_ON_ERROR(result, TAG, "record task failed");

    ESP_LOGI(TAG, "recording stopped, bytes=%lu", (unsigned long)recorded_bytes);
    return ESP_OK;
}

esp_err_t storage_play_recording(void)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "microSD is not mounted");
    ESP_RETURN_ON_FALSE(!storage_record_is_active(), ESP_ERR_INVALID_STATE, TAG, "cannot play while recording");

    FILE *file = fopen(STORAGE_RECORDING_PATH, "rb");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_FAIL, TAG, "open playback failed: errno=%d", errno);

    wav_header_t header = {0};
    if (fread(&header, sizeof(header), 1, file) != 1 || memcmp(header.riff, "RIFF", 4) != 0 ||
        memcmp(header.wave, "WAVE", 4) != 0 || memcmp(header.data, "data", 4) != 0 ||
        header.audio_format != 1 || header.channel_count != 1 || header.bits_per_sample != 16 ||
        header.sample_rate != CONFIG_IPPHONE_I2S_SAMPLE_RATE) {
        fclose(file);
        ESP_LOGE(TAG, "unsupported wav file");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(audio_speaker_start(), TAG, "start speaker for sd playback failed");

    esp_err_t err = ESP_OK;
    uint32_t bytes_remaining = header.data_size;
    int16_t samples[STORAGE_RECORD_CHUNK_SAMPLES] = {0};

    while (bytes_remaining > 0) {
        const size_t samples_to_read =
            bytes_remaining / sizeof(samples[0]) < STORAGE_RECORD_CHUNK_SAMPLES ? bytes_remaining / sizeof(samples[0])
                                                                                 : STORAGE_RECORD_CHUNK_SAMPLES;
        const size_t samples_read = fread(samples, sizeof(samples[0]), samples_to_read, file);
        if (samples_read == 0) {
            err = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }

        err = audio_speaker_write(samples, samples_read);
        if (err != ESP_OK) {
            break;
        }

        bytes_remaining -= (uint32_t)(samples_read * sizeof(samples[0]));
    }

    esp_err_t stop_err = audio_speaker_stop();
    fclose(file);

    if (err == ESP_OK) {
        err = stop_err;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "play recording failed");
    ESP_LOGI(TAG, "recording playback complete");
    return ESP_OK;
}
