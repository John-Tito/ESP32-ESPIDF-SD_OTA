/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// This example uses SPI peripheral to communicate with SD card.

#include <pins_arduino.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char* PROJECT_NAME = "sdOTA";

#define MOUNT_POINT "/sdcard"
#define TEST_DIR "/test"
#define FIRMWARE_DIR "/firmware"

sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t* card;

#define MAX_BUF_SIZE 1024
#define PIN_NUM_MISO SDSPI_MISO
#define PIN_NUM_MOSI SDSPI_MOSI
#define PIN_NUM_CLK SDSPI_SCLK
#define PIN_NUM_CS SDSPI_CS

#define SPI_DMA_CHAN 1

// Options for mounting the filesystem.
// If format_if_mount_failed is set to true, SD card will be partitioned and
// formatted in case when mounting fails.
#define CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED false
static char buf[MAX_BUF_SIZE];
void USER_ESP_LOGI(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    printf("%s:%s\r\n", tag, buf);
    va_end(ap);
}

esp_err_t sdcard_init() {
    esp_err_t ret;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    USER_ESP_LOGI(PROJECT_NAME, "Initializing SD card\n");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.
    USER_ESP_LOGI(PROJECT_NAME, "Using SPI peripheral\n");

    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        USER_ESP_LOGI(PROJECT_NAME, "Failed to initialize bus.\n");
        return -1;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    USER_ESP_LOGI(PROJECT_NAME, "Mounting filesystem\n");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            USER_ESP_LOGI(PROJECT_NAME, "Failed to mount filesystem.\n");
        } else {
            USER_ESP_LOGI(PROJECT_NAME, "Failed to initialize the card.\n");
        }
        return -1;
    }
    USER_ESP_LOGI(PROJECT_NAME, "Filesystem mounted\n");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);
    return 0;
}

void sdcard_deinit() {
    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    USER_ESP_LOGI(PROJECT_NAME, "Card unmounted\n");

    //deinitialize the bus after all devices are removed
    spi_bus_free(host.slot);
}

esp_err_t sdcard_test() {
    const char* file_hello = MOUNT_POINT TEST_DIR "/hello.txt";
    const char* file_foo = MOUNT_POINT TEST_DIR "/foo.txt";
    FILE* f;
    struct stat st;
    char line[64];

    // First create a file.
    USER_ESP_LOGI(PROJECT_NAME, "Opening file %s\n", file_hello);
    f = fopen(file_hello, "w");
    if (f == NULL) {
        USER_ESP_LOGI(PROJECT_NAME, "Failed to open file for writing\n");
        return -1;
    }
    fprintf(f, "Hello %s!\n", card->cid.name);
    fclose(f);
    USER_ESP_LOGI(PROJECT_NAME, "File written\n");

    // Check if destination file exists before renaming
    if (stat(file_foo, &st) == 0) {
        unlink(file_foo);  // Delete it if it exists
    }

    // Rename original file
    USER_ESP_LOGI(PROJECT_NAME, "Renaming file %s to %s\n", file_hello, file_foo);
    if (rename(file_hello, file_foo) != 0) {
        USER_ESP_LOGI(PROJECT_NAME, "Rename failed\n");
        return -1;
    }

    // Open renamed file for reading
    USER_ESP_LOGI(PROJECT_NAME, "Reading file %s\n", file_foo);
    f = fopen(file_foo, "r");
    if (f == NULL) {
        USER_ESP_LOGI(PROJECT_NAME, "Failed to open file for reading\n");
        return -1;
    }

    // Read a line from file
    fgets(line, sizeof(line), f);
    fclose(f);

    // Strip newline
    char* pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    USER_ESP_LOGI(PROJECT_NAME, "Read from file: '%s'\n", line);

    return 0;
}

esp_err_t sdcard_ota() {
    //准备工作

    const char* fimware_file = MOUNT_POINT FIRMWARE_DIR "/update.bin";
    esp_err_t err = ESP_OK;
    FILE* f;
    struct stat st;
    char* line;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t* update_partition = NULL;
    int size_line = 0, size_sum = 0;

    //检查是否为文件
    if (stat(fimware_file, &st) != 0) {
        USER_ESP_LOGI(PROJECT_NAME, "firmware doesn't exit\n");
        return -1;
    }

    if (S_IFDIR & st.st_mode) {
        USER_ESP_LOGI(PROJECT_NAME, "is folder\n");
        return -1;
    }

    if (S_IFREG & st.st_mode) {
        /* 打开文件 */
        f = fopen(fimware_file, "rb");
        if (f == NULL) {
            USER_ESP_LOGI(PROJECT_NAME, "Failed to open file for reading");
            return -1;
        }

        /* 申请1KB内存 */
        line = malloc(1024);
        if (NULL == line) {
            USER_ESP_LOGI(PROJECT_NAME, "malloc faild\n");
            return -1;
        }

        /* 获取未使用的OTA分区指针 */
        update_partition = esp_ota_get_next_update_partition(NULL);
        err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);

        //从SD卡读取数据写入到OTA分区
        while (feof(f)) {
            size_line = fread(line, 1024, 1, f);
            err = esp_ota_write(update_handle, (const void*)line, size_line);
            size_sum += size_line;
        }

        /* 检查文件大小与读取字节数是否一致 */
        if (size_sum != st.st_size) {
            USER_ESP_LOGI(PROJECT_NAME, "file size err\n");
            esp_ota_abort(update_handle);
            return -1;
        }

        //接收完成后收尾工作
        err = esp_ota_end(update_handle);
        if (ESP_OK != err) {
            USER_ESP_LOGI(PROJECT_NAME, "app image invalid\n");
            esp_ota_abort(update_handle);
            return -1;
        }

        err = esp_ota_set_boot_partition(update_partition);
        if (ESP_OK != err) {
            USER_ESP_LOGI(PROJECT_NAME, "switch partition faild\n");
            esp_ota_abort(update_handle);
            return -1;
        }

        sdcard_deinit();
        //软件复位
        esp_restart();
        return 0;
    }
    return 0;
}

void app_main(void) {
    esp_err_t ret;
    const char* need_update_file = MOUNT_POINT "/boot_cnt";
    struct stat st;

    vTaskDelay(5000 / portTICK_PERIOD_MS);

    ret = sdcard_init();
    if (0 != ret) {
        return;
    }

    ret = sdcard_test();
    if (0 != ret) {
        return;
    }

    if (stat(need_update_file, &st) == 0) {
        USER_ESP_LOGI(PROJECT_NAME, "need up date\n");
        unlink(need_update_file);
        sdcard_ota();
    } else {
        USER_ESP_LOGI(PROJECT_NAME, "no need to up date\n");
    }

    sdcard_deinit();

    gpio_pad_select_gpio(BUILTIN_LED);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BUILTIN_LED, GPIO_MODE_OUTPUT);
    while (1) {
        /* Blink off (output low) */
        USER_ESP_LOGI(PROJECT_NAME, "Turning off the LED\n");
        gpio_set_level(BUILTIN_LED, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        /* Blink on (output high) */
        USER_ESP_LOGI(PROJECT_NAME, "Turning on the LED\n");
        gpio_set_level(BUILTIN_LED, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}