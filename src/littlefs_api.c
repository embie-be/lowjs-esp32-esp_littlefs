/**
 * @file littlefs_api.c
 * @brief Maps the HAL of esp_partition <-> littlefs
 * @author Brian Pugh
 */

#define ESP_LOCAL_LOG_LEVEL ESP_LOG_INFO

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "littlefs/lfs.h"
#include "esp_littlefs.h"
#include "littlefs_api.h"

#include "data_spiflash.h"
#include "config.h"

extern int gFSPos;

static const char TAG[] = "esp_littlefs_api";

int littlefs_api_read(const struct lfs_config *c, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    esp_littlefs_t * efs = c->context;
    size_t part_off = (block * c->block_size) + off;

#ifndef CONFIG_NEONIOUS_ONE
    if(efs->internal_version)
    {
        esp_err_t err = spi_flash_read(gFSPos + part_off, buffer, size);
        if (err) {
            ESP_LOGE(TAG, "failed to read addr %08x, size %08x, err %d", part_off, size, err);
            return LFS_ERR_IO;
        }
        return 0;
    }
#endif /* CONFIG_NEONIOUS_ONE */

    data_spiflash_read(part_off + CONFIG_CLIENT_SIZE_DATA_OFFSET, buffer, size);
    return 0;
}

int littlefs_api_prog(const struct lfs_config *c, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    esp_littlefs_t * efs = c->context;
    size_t part_off = (block * c->block_size) + off;

#ifndef CONFIG_NEONIOUS_ONE
    if(efs->internal_version)
    {
        esp_err_t err = spi_flash_write(gFSPos + part_off, buffer, size);
        if (err) {
            ESP_LOGE(TAG, "failed to write addr %08x, size %08x, err %d", part_off, size, err);
            return LFS_ERR_IO;
        }
        return 0;
    }
#endif /* CONFIG_NEONIOUS_ONE */

    data_spiflash_write(part_off + CONFIG_CLIENT_SIZE_DATA_OFFSET, buffer, size);
    return 0;
}

int littlefs_api_erase(const struct lfs_config *c, lfs_block_t block) {
    esp_littlefs_t * efs = c->context;
    size_t part_off = block * c->block_size;

#ifndef CONFIG_NEONIOUS_ONE
    if(efs->internal_version)
    {
        esp_err_t err = spi_flash_erase_range(gFSPos + part_off, c->block_size);
        if (err) {
            ESP_LOGE(TAG, "failed to erase addr %08x, size %08x, err %d", part_off, c->block_size, err);
            return LFS_ERR_IO;
        }
        return 0;
    }
#endif /* CONFIG_NEONIOUS_ONE */

    data_spiflash_erase(part_off + CONFIG_CLIENT_SIZE_DATA_OFFSET, c->block_size);
    return 0;
}

int littlefs_api_sync(const struct lfs_config *c) {
    /* Unnecessary for esp-idf */
    return 0;
}

