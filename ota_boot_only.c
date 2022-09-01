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
#include "mbedtls/sha256.h"
#include "esp_spi_flash.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"

#define HASH_LEN 32 /* SHA-256 digest length */
#define BOOTLOADER_OFFSET       0x1000
#define BOOTLOADER_PART_SIZE    (CONFIG_PARTITION_TABLE_OFFSET - BOOTLOADER_OFFSET)

static const char *TAG = "ota_boot";

extern const uint8_t new_bootloader_start[] asm("_binary_bootloader_bin_start");
extern const uint8_t new_bootloader_end[] asm("_binary_bootloader_bin_end");

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

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
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

    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay 1s
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());

    // get sha256 digest for old bootloader
    uint8_t old_sha_256[HASH_LEN] = { 0 };
    mbedtls_sha256_context sha256_ctx;
    esp_partition_t partition;

    partition.address   = BOOTLOADER_OFFSET;
    partition.size      = new_bootloader_end - new_bootloader_start;
    partition.type      = ESP_PARTITION_TYPE_APP;

    spi_flash_mmap_handle_t old_bootloader_map;
    const void *result = NULL;
    err = esp_partition_mmap(&partition, 0, partition.size, SPI_FLASH_MMAP_DATA, &result, &old_bootloader_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap old bootloader filed. Err=0x%8x", err);
        return;
    }

    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts_ret(&sha256_ctx, false);
    mbedtls_sha256_update_ret(&sha256_ctx, result, new_bootloader_end - new_bootloader_start);
    mbedtls_sha256_finish_ret(&sha256_ctx, old_sha_256);
    spi_flash_munmap(old_bootloader_map);
    print_sha256(old_sha_256, "SHA-256 for Old bootloader: ");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());

    // get sha256 digest for new bootloader
    uint8_t new_sha_256[HASH_LEN] = { 0 };

    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts_ret(&sha256_ctx, false);
    mbedtls_sha256_update_ret(&sha256_ctx, new_bootloader_start, new_bootloader_end - new_bootloader_start);
    mbedtls_sha256_finish_ret(&sha256_ctx, new_sha_256);
    print_sha256(new_sha_256, "SHA-256 for New bootloader: ");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());

    /**
     * @brief Update Bootloader.
     */
    if (memcmp(old_sha_256, new_sha_256, HASH_LEN) != 0) {
        ESP_LOGE(TAG, "The two Bootloader firmware are different, then update Bootloader");
        err = update_bootloader();
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "update Bootloader completed");
        }
    } else {
        ESP_LOGW(TAG, "The two Bootloader firmware are equal.");
    }
    
    /**
     * @brief Rollback to old app bin.
     */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *old_app_partition = esp_ota_get_next_update_partition(NULL);

    ESP_LOGI(TAG, "Running partition type %d subtype %d at offset 0x%x",
             running->type, running->subtype, running->address);
    ESP_LOGI(TAG, "Old App partition type %d subtype %d at offset 0x%x",
             old_app_partition->type, old_app_partition->subtype, old_app_partition->address);

    err = esp_ota_set_boot_partition(old_app_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Restart……");
    vTaskDelay(pdMS_TO_TICKS(100)); // Delay 100ms
    esp_restart();

    return;
}
