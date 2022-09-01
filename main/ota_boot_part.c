/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"

/* For OLD-WiFi-Mesh light Upgrade */
#ifndef CONFIG_UPGRADE_FIRMWARE_FLAG
#define CONFIG_UPGRADE_FIRMWARE_FLAG   "** MUPGRADE_FIRMWARE_FLAG **"
#endif

#define UPGRADE_FIRMWARE_FLAG           CONFIG_UPGRADE_FIRMWARE_FLAG
#define UPGRADE_FIRMWARE_FLAG_SIZE      32

static const char *TAG = "ota_boot_part";

#define BOOTLOADER_OFFSET       0x1000
#define BOOTLOADER_PART_SIZE    (CONFIG_PARTITION_TABLE_OFFSET - BOOTLOADER_OFFSET)

extern const uint8_t new_bootloader_start[] asm("_binary_bootloader_bin_start");
extern const uint8_t new_bootloader_end[] asm("_binary_bootloader_bin_end");

extern const uint8_t new_partitions_start[] asm("_binary_partition_table_bin_start");
extern const uint8_t new_partitions_end[] asm("_binary_partition_table_bin_end");

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

#define OTA_URL_SIZE 256

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );

    /* Ensure to disable any WiFi power save mode, this allows best throughput
     * and hence timings for overall OTA operation. */
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void simple_ota_example_task(void *pvParameter)
{
    /* Wait for the callback to set the CONNECTED_BIT in the
       event group.
    */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Starting OTA example");
    ESP_LOGI(TAG, "Connected to WiFi network! Attempting to connect to server...");
    ESP_LOGI(TAG, "OTA URL: %s", CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL);

    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
        .event_handler = _http_event_handler,
    };

    esp_err_t ret = esp_https_ota(&config);
    if (ret == ESP_OK) {
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

esp_err_t update_bootloader()
{
    // uint8_t boot_read_buff[32] = {0};
    // ESP_ERROR_CHECK(spi_flash_read(BOOTLOADER_OFFSET, boot_read_buff, sizeof(boot_read_buff)));

    ESP_LOGW(TAG, "update Bootloader starting……");
    // if (boot_read_buff[3] != 0x20) { // SPI Flash Freq != 40MHz
        // erase bootloader
        ESP_LOGI(TAG, "erase bootloader partition: %dKB", BOOTLOADER_PART_SIZE / 1024);
        ESP_ERROR_CHECK(spi_flash_erase_range(BOOTLOADER_OFFSET, BOOTLOADER_PART_SIZE));
        // write new bootloader
        if (esp_flash_encryption_enabled()) {
            ESP_LOGI(TAG, "write new bootloader binary with flash encryption enable: %dKB", (new_bootloader_end - new_bootloader_start) / 1024);
            ESP_ERROR_CHECK(spi_flash_write_encrypted(BOOTLOADER_OFFSET, new_bootloader_start, new_bootloader_end - new_bootloader_start));
        } else {
            ESP_LOGI(TAG, "write new bootloader binary: %dKB", (new_bootloader_end - new_bootloader_start) / 1024);
            ESP_ERROR_CHECK(spi_flash_write(BOOTLOADER_OFFSET, new_bootloader_start, new_bootloader_end - new_bootloader_start));
        }

        return ESP_OK;
    // }

    return EALREADY;
}

esp_err_t update_partition_table()
{
    uint8_t boot_read_buff[32] = {0};
    ESP_ERROR_CHECK(spi_flash_read(0x1000, boot_read_buff, sizeof(boot_read_buff)));

    if (boot_read_buff[3] != 0x20) { // SPI Flash Freq != 40MHz
        // erase bootloader
        ESP_LOGI(TAG, "erase bootloader: 28KB");
        ESP_ERROR_CHECK(spi_flash_erase_range(0x1000, 28 * 1024));
        ESP_LOGI(TAG, "write bootloader...");
        ESP_ERROR_CHECK(spi_flash_write(0x1000, bootloader_hex_buff, sizeof(bootloader_hex_buff)));

        return ESP_OK;
    }

    uint8_t part_read_buff[32] = {0};

    ESP_LOGD(TAG, "read partition table");
    ESP_ERROR_CHECK(spi_flash_read(0x8000 + 0x00C0, part_read_buff, sizeof(part_read_buff)));

    /**< Get partition info of currently running app
    Return the next OTA app partition which should be written with a new firmware.*/
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "Running partition, label: %s, type: 0x%x, subtype: 0x%x, address: 0x%x, size: 0x%x",
             running->label, running->type, running->subtype, running->address, running->size);

    if (part_read_buff[3] == 0x03) {
        ESP_LOGD(TAG, "already update partition table");
        return EALREADY;
    }

    if (running->address == 0xc0000) {
        ESP_LOGE(TAG, "\n\nCurrent run ota_0: 0xc0000 partition\n");
        return ESP_FAIL;
    }

    if (running->address == 0x10000) {
        ESP_LOGI(TAG, "\n\nCurrent run ota_0: 0x10000 partition\n");
        return ESP_FAIL;
    }

    // mesh light partition table
    // 000000c0: AA 50 01 9B 00 C0 01 00 00 40 01 00 72 65 73 65    *P...@...@..rese
    // 000000d0: 72 76 65 64 00 00 00 00 00 00 00 00 00 00 00 00    rved............
    if (part_read_buff[3] == 0x9B && running->address == 0x260000) { // part_read_buff[3] != 0x03
        ESP_LOGE(TAG, "\n\nupdate partition table, Please keep the power supply.\n");
        ESP_LOGD(TAG, "partition table address: 0x%04x, sector size: 0x%04x", CONFIG_PARTITION_TABLE_OFFSET, SPI_FLASH_SEC_SIZE);

        // erase partition table
        ESP_LOGD(TAG, "erase partition table: 4KB");
        ESP_ERROR_CHECK(spi_flash_erase_range(0x8000, 4 * 1024));
        ESP_LOGI(TAG, "write partition table...");
        ESP_ERROR_CHECK(spi_flash_write(0x8000, partition_hex_buff, sizeof(partition_hex_buff)));

        // erase bootloader
        ESP_LOGD(TAG, "erase bootloader: 28KB");
        ESP_ERROR_CHECK(spi_flash_erase_range(0x1000, 28 * 1024));
        ESP_LOGI(TAG, "write bootloader...");
        ESP_ERROR_CHECK(spi_flash_write(0x1000, bootloader_hex_buff, sizeof(bootloader_hex_buff)));

        // erase "nvs" nvs partition
        // ESP_LOGD(TAG, "erase default nvs partition");
        // ESP_ERROR_CHECK(spi_flash_erase_range(0x9000, 16 * 1024));

        // erase "otadata" partition
        ESP_LOGI(TAG, "erase otadata partition");
        ESP_ERROR_CHECK(spi_flash_erase_range(0xd000, 8 * 1024));

        // erase "phy_init" partition
        // ESP_LOGI(TAG, "erase phy_init partition");
        // ESP_ERROR_CHECK(spi_flash_erase_range(0xf000, 4 * 1024));

        // erase "ble_mesh" nvs partition
        // ESP_LOGI(TAG, "erase ble_mesh partition");
        // ESP_ERROR_CHECK(spi_flash_erase_range(0x1c000, 64 * 1024));

        // erase "coredump" partition
        // ESP_LOGI(TAG, "erase coredump partition");
        // ESP_ERROR_CHECK(spi_flash_erase_range(0x3dc000, 64 * 1024));

        // erase "reserved" partition
        // ESP_LOGI(TAG, "erase reserved partition");
        // ESP_ERROR_CHECK(spi_flash_erase_range(0x3ec000, 64 * 1024));

        // erase new ota_0(0x10000) partition
        ESP_LOGI(TAG, "erase new ota_0(0x10000) partition");
        ESP_ERROR_CHECK(spi_flash_erase_range(0x10000, 1920 * 1024));

        // write app to new ota_0(0x10000)
        uint32_t writen_size = 0;
        uint8_t *app_read_buff = malloc(1030);
        ESP_LOGI(TAG, "write app to ota_0(0x10000) ...");
        while (writen_size < running->size) {
            ESP_ERROR_CHECK(spi_flash_read(0x260000 + writen_size, app_read_buff, 1024));
            ESP_ERROR_CHECK(spi_flash_write(0x10000 + writen_size, app_read_buff, 1024));
            writen_size += 1024;
            ESP_LOGD(TAG, "Written length: 0x%x", writen_size);
        }

        return ESP_OK;
    } else {
        ESP_LOGD(TAG, "already update partition table");
        return EALREADY;
    }

    return ESP_FAIL;
}

__attribute((constructor)) esp_err_t add_identifier_flag()
{
    esp_err_t err = ESP_OK;
    const volatile uint8_t firmware_flag[UPGRADE_FIRMWARE_FLAG_SIZE] = UPGRADE_FIRMWARE_FLAG;

    (void)firmware_flag;
    ESP_LOGW(TAG, "Add an identifier to the firmware: %s", firmware_flag);

    return ESP_OK;
}

void app_main()
{
    ESP_LOGI(TAG, "app_main");
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 1.OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // 2.NVS partition contains data in new format and cannot be recognized by this version of code.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    vTaskDelay(pdMS_TO_TICKS(20 * 1000)); // Delay 10s

    ESP_LOGW(TAG, "update partition table starting……");
    /**
     * @brief Update Partition Table For Mesh Kit Light.
     */
    err = update_partition_table();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "update partition table completed, Restart……");

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        esp_ota_img_states_t ota_state = 0x00;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                vTaskDelay(pdMS_TO_TICKS(10 * 1000)); // Delay 10s
                esp_ota_mark_app_valid_cancel_rollback();
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(100)); // Delay 100ms
        esp_restart();
        return;
    } else if (err == EALREADY) {
        ESP_LOGI(TAG, "Partition Table:");
        ESP_LOGI(TAG, "## Label            Usage          Type ST Offset   Length");

        uint8_t i = 0;
        const char *partition_usage = "test";
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t *partition = esp_partition_get(it);
            
            /* valid partition table */
            switch(partition->type) {
            case PART_TYPE_APP: /* app partition */
                switch(partition->subtype) {
                case PART_SUBTYPE_FACTORY: /* factory binary */
                    partition_usage = "factory app";
                    break;
                case PART_SUBTYPE_TEST: /* test binary */
                    partition_usage = "test app";
                    break;
                default:
                    /* OTA binary */
                    if ((partition->subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG) {
                        partition_usage = "OTA app";
                    }
                    else {
                        partition_usage = "Unknown app";
                    }
                    break;
                }
                break; /* PART_TYPE_APP */
            case PART_TYPE_DATA: /* data partition */
                switch(partition->subtype) {
                case PART_SUBTYPE_DATA_OTA: /* ota data */
                    partition_usage = "OTA data";
                    break;
                case PART_SUBTYPE_DATA_RF:
                    partition_usage = "RF data";
                    break;
                case PART_SUBTYPE_DATA_WIFI:
                    partition_usage = "WiFi data";
                    break;
                case PART_SUBTYPE_DATA_NVS_KEYS:
                    partition_usage = "NVS keys";
                    break;
                case PART_SUBTYPE_DATA_EFUSE_EM:
                    partition_usage = "efuse";
                    break;
                default:
                    partition_usage = "Unknown data";
                    break;
                }
                break; /* PARTITION_USAGE_DATA */
            default: /* other partition type */
                break;
            }

            /* print partition type info */
            ESP_LOGI(TAG, "%2d %-16s %-16s %02x %02x %08x %08x", i++, partition->label, partition_usage,
                    partition->type, partition->subtype, partition->address, partition->size);
        }

        it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t *partition = esp_partition_get(it);
            
            /* valid partition table */
            switch(partition->type) {
            case PART_TYPE_APP: /* app partition */
                switch(partition->subtype) {
                case PART_SUBTYPE_FACTORY: /* factory binary */
                    partition_usage = "factory app";
                    break;
                case PART_SUBTYPE_TEST: /* test binary */
                    partition_usage = "test app";
                    break;
                default:
                    /* OTA binary */
                    if ((partition->subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG) {
                        partition_usage = "OTA app";
                    }
                    else {
                        partition_usage = "Unknown app";
                    }
                    break;
                }
                break; /* PART_TYPE_APP */
            case PART_TYPE_DATA: /* data partition */
                switch(partition->subtype) {
                case PART_SUBTYPE_DATA_OTA: /* ota data */
                    partition_usage = "OTA data";
                    break;
                case PART_SUBTYPE_DATA_RF:
                    partition_usage = "RF data";
                    break;
                case PART_SUBTYPE_DATA_WIFI:
                    partition_usage = "WiFi data";
                    break;
                case PART_SUBTYPE_DATA_NVS_KEYS:
                    partition_usage = "NVS keys";
                    break;
                case PART_SUBTYPE_DATA_EFUSE_EM:
                    partition_usage = "efuse";
                    break;
                default:
                    partition_usage = "Unknown data";
                    break;
                }
                break; /* PARTITION_USAGE_DATA */
            default: /* other partition type */
                break;
            }

            /* print partition type info */
            ESP_LOGI(TAG, "%2d %-16s %-16s %02x %02x %08x %08x", i++, partition->label, partition_usage,
                    partition->type, partition->subtype, partition->address, partition->size);
        }
    }

    initialise_wifi();

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_ota_img_states_t ota_state = 0x00;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            vTaskDelay(pdMS_TO_TICKS(10 * 1000)); // Delay 10s
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
#endif

    xTaskCreate(&simple_ota_example_task, "ota_example_task", 8192, NULL, 5, NULL);
}
