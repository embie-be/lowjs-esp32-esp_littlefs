/**
 * @file esp_littlefs.c
 * @brief Maps LittleFS <-> ESP_VFS 
 * @author Brian Pugh
 */

//#define LOG_LOCAL_LEVEL 4

#include "esp_log.h"
#include "esp_spi_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <unistd.h>
#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "esp32/rom/spi_flash.h"
#include "esp_system.h"

#include "esp_littlefs.h"
#include "littlefs_api.h"

#include "data_spiflash.h"
#include "config.h"
#include "alloc.h"


static const char TAG[] = "esp_littlefs";

#define CONFIG_LITTLEFS_BLOCK_SIZE 4096 /* ESP32 can only operate at 4kb */

/* File Descriptor Caching Params */
#define CONFIG_LITTLEFS_FD_CACHE_REALLOC_FACTOR 2  /* Amount to resize FD cache by */
#define CONFIG_LITTLEFS_FD_CACHE_MIN_SIZE 4  /* Minimum size of FD cache */
#define CONFIG_LITTLEFS_FD_CACHE_HYST 4  /* When shrinking, leave this many trailing FD slots available */

/**
 * @brief littlefs DIR structure
 */
typedef struct {
    DIR dir;            /*!< VFS DIR struct */
    lfs_dir_t d;        /*!< littlefs DIR struct */
    struct dirent e;    /*!< Last open dirent */
    long offset;        /*!< Offset of the current dirent */
    char *path;         /*!< Requested directory name */
} vfs_littlefs_dir_t;

static int     vfs_littlefs_open(void* ctx, const char * path, int flags, int mode);
static ssize_t vfs_littlefs_write(void* ctx, int fd, const void * data, size_t size);
static ssize_t vfs_littlefs_read(void* ctx, int fd, void * dst, size_t size);
static int     vfs_littlefs_close(void* ctx, int fd);
static off_t   vfs_littlefs_lseek(void* ctx, int fd, off_t offset, int mode);
static int     vfs_littlefs_stat(void* ctx, const char * path, struct stat * st);
static int     vfs_littlefs_unlink(void* ctx, const char *path);
static int     vfs_littlefs_rename(void* ctx, const char *src, const char *dst);
static DIR*    vfs_littlefs_opendir(void* ctx, const char* name);
static int     vfs_littlefs_closedir(void* ctx, DIR* pdir);
static struct  dirent* vfs_littlefs_readdir(void* ctx, DIR* pdir);
static int     vfs_littlefs_readdir_r(void* ctx, DIR* pdir,
                                struct dirent* entry, struct dirent** out_dirent);
static long    vfs_littlefs_telldir(void* ctx, DIR* pdir);
static void    vfs_littlefs_seekdir(void* ctx, DIR* pdir, long offset);
static int     vfs_littlefs_mkdir(void* ctx, const char* name, mode_t mode);
static int     vfs_littlefs_rmdir(void* ctx, const char* name);
static int     vfs_littlefs_fsync(void* ctx, int fd);

static esp_err_t esp_littlefs_init(const esp_vfs_littlefs_conf_t* conf);
static esp_err_t esp_littlefs_by_label(const char* label, int * index);
static esp_err_t esp_littlefs_get_empty(int *index);
static void      esp_littlefs_free(esp_littlefs_t ** efs);
static void      esp_littlefs_dir_free(vfs_littlefs_dir_t *dir);
static int       esp_littlefs_flags_conv(int m);
#if CONFIG_LITTLEFS_USE_MTIME
static int       vfs_littlefs_utime(void *ctx, const char *path, const struct utimbuf *times);
static void      vfs_littlefs_update_mtime(esp_littlefs_t *efs, const char *path);
static int       vfs_littlefs_update_mtime_value(esp_littlefs_t *efs, const char *path, time_t t);
static time_t    vfs_littlefs_get_mtime(esp_littlefs_t *efs, const char *path);
#endif

#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
/* The only way in LittleFS to get info is via a path (lfs_stat), so it cannot
 * be done if the path isn't stored. */
static int     vfs_littlefs_fstat(void* ctx, int fd, struct stat * st);
#endif

static int sem_take(esp_littlefs_t *efs);
static int sem_give(esp_littlefs_t *efs);

static SemaphoreHandle_t _efs_lock = NULL;
static esp_littlefs_t * _efs[CONFIG_LITTLEFS_MAX_PARTITIONS] = { 0 };

extern uint32_t gSPIFlashSize;
extern int gFSPos;

/********************
 * Helper Functions *
 ********************/
void esp_littlefs_free_fds(esp_littlefs_t * efs) {
    /* Need to free all files that were opened */
    while (efs->file) {
        vfs_littlefs_file_t * next = efs->file->next;
        free(efs->file);
        efs->file = next;
    }
    free(efs->cache); 
    efs->cache = 0;
    efs->cache_size = efs->fd_count = 0;
}


/********************
 * Public Functions *
 ********************/

bool esp_littlefs_mounted(const char* partition_label) {
    int index;
    esp_err_t err;

    err = esp_littlefs_by_label(partition_label, &index);
    if(err != ESP_OK) return false;
    return _efs[index]->cache_size > 0;
}

esp_err_t esp_littlefs_info(const char* partition_label, size_t *total_bytes, size_t *used_bytes){
    int index;
    esp_err_t err;
    esp_littlefs_t *efs = NULL;

    err = esp_littlefs_by_label(partition_label, &index);
    if(err != ESP_OK) return false;
    efs = _efs[index];

    sem_take(efs);
    if(total_bytes) *total_bytes = efs->cfg.block_size * efs->cfg.block_count; 
    if(used_bytes) *used_bytes = efs->cfg.block_size * lfs_fs_size(efs->fs);
    sem_give(efs);

    return ESP_OK;
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t * conf)
{
    assert(conf->base_path);
    const esp_vfs_t vfs = {
        .flags       = ESP_VFS_FLAG_CONTEXT_PTR,
        .write_p     = &vfs_littlefs_write,
        .lseek_p     = &vfs_littlefs_lseek,
        .read_p      = &vfs_littlefs_read,
        .open_p      = &vfs_littlefs_open,
        .close_p     = &vfs_littlefs_close,
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
        .fstat_p     = &vfs_littlefs_fstat,
#else
        .fstat_p     = NULL, /* Not supported */
#endif
        .stat_p      = &vfs_littlefs_stat,
        .link_p      = NULL, /* Not Supported */
        .unlink_p    = &vfs_littlefs_unlink,
        .rename_p    = &vfs_littlefs_rename,
        .opendir_p   = &vfs_littlefs_opendir,
        .closedir_p  = &vfs_littlefs_closedir,
        .readdir_p   = &vfs_littlefs_readdir,
        .readdir_r_p = &vfs_littlefs_readdir_r,
        .seekdir_p   = &vfs_littlefs_seekdir,
        .telldir_p   = &vfs_littlefs_telldir,
        .mkdir_p     = &vfs_littlefs_mkdir,
        .rmdir_p     = &vfs_littlefs_rmdir,
        .fsync_p     = &vfs_littlefs_fsync,
#if CONFIG_LITTLEFS_USE_MTIME
        .utime_p     = &vfs_littlefs_utime,
#else
        .utime_p     = NULL,
#endif // CONFIG_LITTLEFS_USE_MTIME
    };

    esp_err_t err = esp_littlefs_init(conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS");
        return err;
    }

    int index;
    if (esp_littlefs_by_label(conf->partition_label, &index) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to find partition \"%s\"", conf->partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    strlcat(_efs[index]->base_path, conf->base_path, ESP_VFS_PATH_MAX + 1);
    err = esp_vfs_register(conf->base_path, &vfs, _efs[index]);
    if (err != ESP_OK) {
        esp_littlefs_free(&_efs[index]);
        ESP_LOGE(TAG, "Failed to register Littlefs to \"%s\"", conf->base_path);
        return err;
    }

    ESP_LOGD(TAG, "Successfully registered LittleFS to \"%s\"", conf->base_path);
    return ESP_OK;
}

esp_err_t esp_vfs_littlefs_unregister(const char* partition_label)
{
    assert(partition_label);
    int index;
    if (esp_littlefs_by_label(partition_label, &index) != ESP_OK) {
        ESP_LOGE(TAG, "Partition was never registered.");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGD(TAG, "Unregistering \"%s\"", partition_label);
    esp_err_t err = esp_vfs_unregister(_efs[index]->base_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister \"%s\"", partition_label);
        return err;
    }
    esp_littlefs_free(&_efs[index]);
    _efs[index] = NULL;
    return ESP_OK;
}

esp_err_t esp_littlefs_format(bool internal_version) {
    bool was_mounted = false;
    bool efs_free = false;
    int index = -1;
    esp_err_t err;
    esp_littlefs_t *efs = NULL;
    const char *partition_label = internal_version ? "internal" : "external";

    ESP_LOGI(TAG, "Formatting %s", partition_label);

    /* Get a context */
    err = esp_littlefs_by_label(partition_label, &index);

    if( err != ESP_OK ){
        /* Create a tmp context */
        ESP_LOGD(TAG, "Temporarily creating EFS context.");
        efs_free = true;
        const esp_vfs_littlefs_conf_t conf = {
                /* base_name not necessary for initializing */
                .dont_mount = true, 
                .partition_label = partition_label,
        };
        err = esp_littlefs_init(&conf); /* Internally MIGHT call esp_littlefs_format */
        if( err != ESP_OK ) {
            ESP_LOGE(TAG, "Failed to initialize to format.");
            goto exit;
        }

        err = esp_littlefs_by_label(partition_label, &index);
        if ( err != ESP_OK) {
            ESP_LOGE(TAG, "Error obtaining context.");
            goto exit;
        }
    }

    efs = _efs[index];
    assert( efs );

    /* Unmount if mounted */
    if(efs->cache_size > 0){
        int res;
        ESP_LOGD(TAG, "Partition was mounted. Unmounting...");
        was_mounted = true;
        res = lfs_unmount(efs->fs);
        if(res != LFS_ERR_OK){
            ESP_LOGE(TAG, "Failed to unmount.");
            return ESP_FAIL;
        }
        esp_littlefs_free_fds(efs);
    }

    /* Erase and Format */
    {
        int res;
        ESP_LOGD(TAG, "Formatting filesystem");
#ifndef CONFIG_NEONIOUS_ONE
        if(internal_version)
            spi_flash_erase_range(gFSPos, g_rom_flashchip.chip_size - gFSPos);
        else
#endif
            data_spiflash_erase(CONFIG_CLIENT_SIZE_DATA_OFFSET, gSPIFlashSize - CONFIG_CLIENT_SIZE_DATA_OFFSET);
        res = lfs_format(efs->fs, &efs->cfg);
        if( res != LFS_ERR_OK ) {
            ESP_LOGE(TAG, "Failed to format filesystem");
            return ESP_FAIL;
        }
    }

    /* Mount filesystem */
    if( was_mounted ) {
        int res;
        /* Remount the partition */
        ESP_LOGD(TAG, "Remounting formatted partition");
        res = lfs_mount(efs->fs, &efs->cfg);
        if( res != LFS_ERR_OK ) {
            ESP_LOGE(TAG, "Failed to re-mount filesystem");
            return ESP_FAIL;
        }
        efs->cache_size = CONFIG_LITTLEFS_FD_CACHE_MIN_SIZE;  // Initial size of cache; will resize ondemand
        efs->cache = low_calloc(sizeof(*efs->cache), efs->cache_size);
    }
    ESP_LOGD(TAG, "Format Success!");
    
    err = ESP_OK;

exit:
    if(efs_free && index>=0) esp_littlefs_free(&_efs[index]);
    return err;
}

#if CONFIG_LITTLEFS_HUMAN_READABLE
/**
 * @brief converts an enumerated lfs error into a string.
 * @param lfs_error The littlefs error.
 */
const char * esp_littlefs_errno(enum lfs_error lfs_errno) {
    switch(lfs_errno){
        case LFS_ERR_OK: return "LFS_ERR_OK";
        case LFS_ERR_IO: return "LFS_ERR_IO";
        case LFS_ERR_CORRUPT: return "LFS_ERR_CORRUPT";
        case LFS_ERR_NOENT: return "LFS_ERR_NOENT";
        case LFS_ERR_EXIST: return "LFS_ERR_EXIST";
        case LFS_ERR_NOTDIR: return "LFS_ERR_NOTDIR";
        case LFS_ERR_ISDIR: return "LFS_ERR_ISDIR";
        case LFS_ERR_NOTEMPTY: return "LFS_ERR_NOTEMPTY";
        case LFS_ERR_BADF: return "LFS_ERR_BADF";
        case LFS_ERR_FBIG: return "LFS_ERR_FBIG";
        case LFS_ERR_INVAL: return "LFS_ERR_INVAL";
        case LFS_ERR_NOSPC: return "LFS_ERR_NOSPC";
        case LFS_ERR_NOMEM: return "LFS_ERR_NOMEM";
        case LFS_ERR_NOATTR: return "LFS_ERR_NOATTR";
        case LFS_ERR_NAMETOOLONG: return "LFS_ERR_NAMETOOLONG";
        default: return "LFS_ERR_UNDEFINED";
    }
    return "";
}
#else
#define esp_littlefs_errno(x) ""
#endif

/********************
 * Static Functions *
 ********************/

/*** Helpers ***/

/**
 * @brief Free and clear a littlefs definition structure.
 * @param efs Pointer to pointer to struct. Done this way so we can also zero
 *            out the pointer.
 */
static void esp_littlefs_free(esp_littlefs_t ** efs)
{
    esp_littlefs_t * e = *efs;
    if (e == NULL) return;
    *efs = NULL;

    if (e->fs) {
        if(e->cache_size > 0) lfs_unmount(e->fs);
        free(e->fs);
    }
    if(e->lock) vSemaphoreDelete(e->lock);
    esp_littlefs_free_fds(e);
    free(e->label);
    free(e);
}

/**
 * @brief Free a vfs_littlefs_dir_t struct.
 */
static void esp_littlefs_dir_free(vfs_littlefs_dir_t *dir){
    if(dir == NULL) return;
    if(dir->path) free(dir->path);
    free(dir);
}

/**
 * Get a mounted littlefs filesystem by label.
 * @param[in] label
 * @param[out] index index into _efs
 * @return ESP_OK on success
 */
static esp_err_t esp_littlefs_by_label(const char* label, int * index){
    int i;
    esp_littlefs_t * p;

    if(!label || !index) return ESP_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "Searching for existing filesystem for partition \"%s\"", label);

    for (i = 0; i < CONFIG_LITTLEFS_MAX_PARTITIONS; i++) {
        p = _efs[i];
        if (p) {
            if (strcmp(label, p->label) == 0) {
                *index = i;
                ESP_LOGD(TAG, "Found existing filesystem \"%s\" at index %d", label, *index);
                return ESP_OK;
            }
        }
    }

    ESP_LOGD(TAG, "Existing filesystem \%s\" not found", label);
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Get the index of an unallocated LittleFS slot.
 * @param[out] index Indexd of free LittleFS slot 
 * @return ESP_OK on success
 */
static esp_err_t esp_littlefs_get_empty(int *index) {
    assert(index);
    for(uint8_t i=0; i < CONFIG_LITTLEFS_MAX_PARTITIONS; i++){
        if( _efs[i] == NULL ){
            *index = i;
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "No more free partitions available.");
    return ESP_FAIL;
}

/**
 * @brief Convert fcntl flags to littlefs flags
 * @param m fcntl flags
 * @return lfs flags
 */
static int esp_littlefs_flags_conv(int m) {
    int lfs_flags = 0;
    if (m == O_APPEND) lfs_flags |= LFS_O_APPEND;
    if (m == O_RDONLY) lfs_flags |= LFS_O_RDONLY;
    if (m & O_WRONLY)  lfs_flags |= LFS_O_WRONLY;
    if (m & O_RDWR)    lfs_flags |= LFS_O_RDWR;
    if (m & O_EXCL)    lfs_flags |= LFS_O_EXCL;
    if (m & O_CREAT)   lfs_flags |= LFS_O_CREAT;
    if (m & O_TRUNC)   lfs_flags |= LFS_O_TRUNC;
    return lfs_flags;
}

/**
 * @brief Initialize and mount littlefs 
 * @param[in] conf Filesystem Configuration
 * @return ESP_OK on success
 */
static esp_err_t esp_littlefs_init(const esp_vfs_littlefs_conf_t* conf)
{
    int index = -1;
    esp_err_t err = ESP_FAIL;
    esp_littlefs_t * efs = NULL;

    if( _efs_lock == NULL ){
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);
        if( _efs_lock == NULL ){
            _efs_lock = xSemaphoreCreateMutex();
            assert(_efs_lock);
        }
        portEXIT_CRITICAL(&mux);
    }

    xSemaphoreTake(_efs_lock, portMAX_DELAY);

    if (esp_littlefs_get_empty(&index) != ESP_OK) {
        ESP_LOGE(TAG, "max mounted partitions reached");
        err = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    /* Input and Environment Validation */
    if (esp_littlefs_by_label(conf->partition_label, &index) == ESP_OK) {
        ESP_LOGE(TAG, "Partition already used");
        err = ESP_ERR_INVALID_STATE;
        goto exit;
    }

#ifdef CONFIG_NEONIOUS_ONE
    bool internal_version = true;

    /* Allocate Context */
    efs = low_calloc(1, sizeof(esp_littlefs_t));
    if (efs == NULL) {
        ESP_LOGE(TAG, "esp_littlefs could not be malloced");
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    // block device configuration
    efs->cfg.read_size = DATA_SPIFLASH_WRITE_256B;
    efs->cfg.prog_size = DATA_SPIFLASH_WRITE_256B;
    efs->cfg.block_size = DATA_SPIFLASH_ERASE_4KB;
    efs->cfg.block_count = (gSPIFlashSize - CONFIG_CLIENT_SIZE_DATA_OFFSET) / efs->cfg.block_size;
    efs->cfg.cache_size = CONFIG_LITTLEFS_CACHE_SIZE;
    efs->cfg.lookahead_size = CONFIG_LITTLEFS_LOOKAHEAD_SIZE;
    efs->cfg.block_cycles = CONFIG_LITTLEFS_BLOCK_CYCLES;
#else
    bool internal_version = conf->partition_label && strcmp(conf->partition_label, "internal") == 0;   
    if(!internal_version)
    {
        /* Allocate Context */
        efs = low_calloc(1, sizeof(esp_littlefs_t));
        if (efs == NULL) {
            ESP_LOGE(TAG, "esp_littlefs could not be malloced");
            err = ESP_ERR_NO_MEM;
            goto exit;
        }

        // block device configuration
        efs->cfg.read_size = DATA_SPIFLASH_WRITE_256B;
        efs->cfg.prog_size = DATA_SPIFLASH_WRITE_256B;
        efs->cfg.block_size = DATA_SPIFLASH_ERASE_4KB;; 
        efs->cfg.block_count = (gSPIFlashSize - CONFIG_CLIENT_SIZE_DATA_OFFSET) / efs->cfg.block_size;
        efs->cfg.cache_size = CONFIG_LITTLEFS_CACHE_SIZE;
        efs->cfg.lookahead_size = CONFIG_LITTLEFS_LOOKAHEAD_SIZE;
        efs->cfg.block_cycles = CONFIG_LITTLEFS_BLOCK_CYCLES;
    }
    else
    {
        {
            uint32_t flash_page_size = g_rom_flashchip.page_size;
            uint32_t log_page_size = CONFIG_LITTLEFS_PAGE_SIZE;
            if (log_page_size % flash_page_size != 0) {
                ESP_LOGE(TAG, "LITTLEFS_PAGE_SIZE is not multiple of flash chip page size (%d)",
                        flash_page_size);
                err = ESP_ERR_INVALID_ARG;
                goto exit;
            }
        }

        if ( NULL == conf->partition_label ) {
            ESP_LOGE(TAG, "Partition label must be provided.");
            err = ESP_ERR_INVALID_ARG;
            goto exit;
        }

        /* Allocate Context */
        efs = low_calloc(1, sizeof(esp_littlefs_t));
        if (efs == NULL) {
            ESP_LOGE(TAG, "esp_littlefs could not be malloced");
            err = ESP_ERR_NO_MEM;
            goto exit;
        }

        // block device configuration
        efs->cfg.read_size = CONFIG_LITTLEFS_READ_SIZE;
        efs->cfg.prog_size = CONFIG_LITTLEFS_WRITE_SIZE;
        efs->cfg.block_size = CONFIG_LITTLEFS_BLOCK_SIZE;
        efs->cfg.block_count = (g_rom_flashchip.chip_size - gFSPos) / efs->cfg.block_size;
        efs->cfg.cache_size = CONFIG_LITTLEFS_CACHE_SIZE;
        efs->cfg.lookahead_size = CONFIG_LITTLEFS_LOOKAHEAD_SIZE;
        efs->cfg.block_cycles = CONFIG_LITTLEFS_BLOCK_CYCLES;
    }
#endif /* CONFIG_NEONIOUS_ONE */
    efs->internal_version = internal_version;
    efs->label = strdup(conf->partition_label);

    { /* LittleFS Configuration */
        efs->cfg.context = efs;

        // block device operations
        efs->cfg.read  = littlefs_api_read;
        efs->cfg.prog  = littlefs_api_prog;
        efs->cfg.erase = littlefs_api_erase;
        efs->cfg.sync  = littlefs_api_sync;
    }

    efs->lock = xSemaphoreCreateRecursiveMutex();
    if (efs->lock == NULL) {
        ESP_LOGE(TAG, "mutex lock could not be created");
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    efs->fs = low_calloc(1, sizeof(lfs_t));
    if (efs->fs == NULL) {
        ESP_LOGE(TAG, "littlefs could not be malloced");
        err = ESP_ERR_NO_MEM;
        goto exit;
    }

    // Mount and Error Check
    _efs[index] = efs;
    if(!conf->dont_mount){
        int res = lfs_mount(efs->fs, &efs->cfg);

        if (conf->format_if_mount_failed && res != LFS_ERR_OK) {
            esp_err_t err;
            ESP_LOGW(TAG, "mount failed, %s (%i). formatting...", esp_littlefs_errno(res), res);
            err = esp_littlefs_format(efs->internal_version);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "format failed");
                err = ESP_FAIL;
                goto exit;
            }
            res = lfs_mount(efs->fs, &efs->cfg);
        }
        if (res != LFS_ERR_OK) {
            ESP_LOGE(TAG, "mount failed, %s (%i)", esp_littlefs_errno(res), res);
            err = ESP_FAIL;
            goto exit;
        }
        efs->cache_size = 4;
        efs->cache = low_calloc(sizeof(*efs->cache), efs->cache_size);
    }

    err = ESP_OK;

exit:
    if(err != ESP_OK){
        if( index >= 0 ) {
            esp_littlefs_free(&_efs[index]);
        }
        else{
            esp_littlefs_free(&efs);
        }
    }
    xSemaphoreGive(_efs_lock);
    return err;
}

/**
 * @brief
 * @parameter efs file system context
 */
static inline int sem_take(esp_littlefs_t *efs) {
#if LOG_LOCAL_LEVEL >= 4
    ESP_LOGD(TAG, "------------------------ Sem Taking [%s]", pcTaskGetTaskName(NULL));
#endif
    xSemaphoreTakeRecursive(efs->lock, portMAX_DELAY);
#if LOG_LOCAL_LEVEL >= 4
    ESP_LOGD(TAG, "--------------------->>> Sem Taken [%s]", pcTaskGetTaskName(NULL));
#endif
    return 0;
}

/**
 * @brief
 * @parameter efs file system context
 */
static inline int sem_give(esp_littlefs_t *efs) {
#if LOG_LOCAL_LEVEL >= 4
    ESP_LOGD(TAG, "---------------------<<< Sem Give [%s]", pcTaskGetTaskName(NULL));
#endif
    return xSemaphoreGiveRecursive(efs->lock);
}


/* We are using a double allocation system here, which an array and a linked list. 
   The array contains the pointer to the file descriptor (the index in the array is what's returned to the user).
   The linked list is used for file descriptors.
   This means that position of nodes in the list must stay consistent:
   - Allocation is obvious (append to the list from the head, and realloc the pointers array)
     There is still a O(N) search in the cache for a free position to store
   - Searching is a O(1) process (good)
   - Deallocation is more tricky. That is, for example, 
     if you need to remove node 5 in a 12 nodes list, you'll have to:
       1) Mark the 5th position as freed (if it's the last position of the array realloc smaller)
       2) Walk the list until finding the pointer to the node O(N) and scrub the node so the chained list stays consistent
       3) Deallocate the node 
*/

/**
 * @brief Get a file descriptor
 * @param[in,out] efs       file system context
 * @param[out]    file      pointer to a file that'll be filled with a file object
 * @param[in]     path_len  the length of the filepath in bytes (including terminating zero byte)
 * @return integer file descriptor. Returns -1 if a FD cannot be obtained.
 * @warning This must be called with lock taken
 */
static int esp_littlefs_allocate_fd(esp_littlefs_t *efs, vfs_littlefs_file_t ** file
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
  , const size_t path_len
#endif
    )
{
    int i = -1;

    assert( efs->fd_count < UINT16_MAX );
    assert( efs->cache_size < UINT16_MAX );

    /* Make sure there is enough space in the cache to store new fd */
    if (efs->fd_count + 1 > efs->cache_size) {
        uint16_t new_size = (uint16_t)MIN(UINT16_MAX, CONFIG_LITTLEFS_FD_CACHE_REALLOC_FACTOR * efs->cache_size);
        /* Resize the cache */
        vfs_littlefs_file_t ** new_cache = realloc(efs->cache, new_size * sizeof(*efs->cache));
        if (!new_cache) {
            ESP_LOGE(TAG, "Unable to allocate file cache");
            return -1; /* If it fails here, no harm is done to the filesystem, so it's safe */
        }
        /* Zero out the new portions of the cache */
        memset(&new_cache[efs->cache_size], 0, (new_size - efs->cache_size) * sizeof(*efs->cache));
        efs->cache = new_cache;
        efs->cache_size = new_size;
    }


    /* Allocate file descriptor here now */
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
    *file = low_calloc(1, sizeof(**file) + path_len);
#else
    *file = low_calloc(1, sizeof(**file));
#endif

    if (*file == NULL) {
        /* If it fails here, the file system might have a larger cache, but it's harmless, no need to reverse it */
        ESP_LOGE(TAG, "Unable to allocate FD");
        return -1; 
    }

    /* Starting from here, nothing can fail anymore */

#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
    /* The trick here is to avoid dual allocation so the path pointer 
        should point to the next byte after it:
        file => [ lfs_file | # | next | path | free_space ]
                                            |  /\
                                            |__/
    */
    (*file)->path = (char*)(*file) + sizeof(**file);
#endif
 
    /* Now find a free place in cache */
    for(i=0; i < efs->cache_size; i++) {
        if (efs->cache[i] == NULL) {
            efs->cache[i] = *file;
            break;
        }
    }
    /* Save file in the list */
    (*file)->next = efs->file;
    efs->file = *file;
    efs->fd_count++;
    return i;
}

/**
 * @brief Release a file descriptor
 * @param[in,out] efs file system context
 * @param[in] fd File Descriptor to release
 * @return 0 on success. -1 if a FD cannot be obtained.
 * @warning This must be called with lock taken
 */
static int esp_littlefs_free_fd(esp_littlefs_t *efs, int fd){
    vfs_littlefs_file_t * file, * head;

    if((uint32_t)fd >= efs->cache_size) {
        ESP_LOGE(TAG, "FD %d must be <%d.", fd, efs->cache_size);
        return -1;
    }

    /* Get the file descriptor to free it */
    file = efs->cache[fd];
    head = efs->file;
    /* Search for file in SLL to remove it */
    if (file == head) {
        /* Last file, can't fail */
        efs->file = efs->file->next;
    } else {
        while (head && head->next != file) {
            head = head->next;
        }
        if (!head) {
            ESP_LOGE(TAG, "Inconsistent list");
            return -1;
        }
        /* Transaction starts here and can't fail anymore */ 
        head->next = file->next;
    }
    efs->cache[fd] = NULL;
    efs->fd_count--;

    ESP_LOGD(TAG, "Clearing FD");
    free(file);

#if 0
    /* Realloc smaller if its possible
     *     * Find and realloc based on number of trailing NULL ptrs in cache
     *     * Leave some hysteris to prevent thrashing around resize points
     * This is disabled for now because it adds unnecessary complexity
     * and binary size increase that outweights its ebenfits.
     */
    if(efs->cache_size > CONFIG_LITTLEFS_FD_CACHE_MIN_SIZE) {
        uint16_t n_free;
        uint16_t new_size = efs->cache_size / CONFIG_LITTLEFS_FD_CACHE_REALLOC_FACTOR;

        if(new_size >= CONFIG_LITTLEFS_FD_CACHE_MIN_SIZE) {
            /* Count number of trailing NULL ptrs */
            for(n_free=0; n_free < efs->cache_size; n_free++) {
                if(efs->cache[efs->cache_size - n_free - 1] != NULL) {
                    break;
                }
            }

            if(n_free >= (efs->cache_size - new_size)){
                new_size += CONFIG_LITTLEFS_FD_CACHE_HYST;
                ESP_LOGD(TAG, "Reallocating cache %i -> %i", efs->cache_size, new_size);
                vfs_littlefs_file_t ** new_cache;
                new_cache = realloc(efs->cache, new_size * sizeof(*efs->cache));
                /* No harm on realloc failure, continue using the oversized cache */
                if(new_cache) {
                    efs->cache = new_cache;
                    efs->cache_size = new_size;
                }
            }
        }
    }
#endif

    return 0;
}

/**
 * @brief Compute the 32bit DJB2 hash of the given string.
 * @param[in]   path the path to hash
 * @returns the hash for this path 
 */
static uint32_t compute_hash(const char * path) {
    uint32_t hash = 5381;
    char c;

    while ((c = *path++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}

/**
 * @brief finds an open file descriptor by file name.
 * @param[in,out] efs file system context
 * @param[in] path File path to check.
 * @returns integer file descriptor. Returns -1 if not found.
 * @warning This must be called with lock taken
 * @warning if CONFIG_LITTLEFS_USE_ONLY_HASH, there is a slim chance an
 *          erroneous FD may be returned on hash collision.
 */
static int esp_littlefs_get_fd_by_name(esp_littlefs_t *efs, const char *path){
    uint32_t hash = compute_hash(path);

    for(uint16_t i=0, j=0; i < efs->cache_size && j < efs->fd_count; i++){
        if (efs->cache[i]) {
            ++j; 

            if (
                efs->cache[i]->hash == hash  // Faster than strcmp
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
                && strcmp(path, efs->cache[i]->path) == 0  // May as well check incase of hash collision. Usually short-circuited.
#endif
            ) {
                ESP_LOGD(TAG, "Found \"%s\" at FD %d.", path, i);
                return i;
            }
        }
    }
    ESP_LOGD(TAG, "Unable to get a find FD for \"%s\"", path);
    return -1;
}

/*** Filesystem Hooks ***/

static int vfs_littlefs_open(void* ctx, const char * path, int flags, int mode) {
    /* Note: mode is currently unused */
    int fd=-1, lfs_flags, res;
    esp_littlefs_t *efs = (esp_littlefs_t *)ctx;
    vfs_littlefs_file_t *file = NULL;
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
    size_t path_len = strlen(path) + 1;  // include NULL terminator
#endif
    assert(path);

    ESP_LOGD(TAG, "Opening %s", path);

    /* Convert flags to lfs flags */
    lfs_flags = esp_littlefs_flags_conv(flags);

    /* Get a FD */
    sem_take(efs);
    fd = esp_littlefs_allocate_fd(efs, &file
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
    , path_len
#endif
    );
    if(fd < 0) {
        sem_give(efs);
        ESP_LOGE(TAG, "Error obtaining FD");
        errno = -LFS_ERR_INVAL;
        return -1;
    }
    /* Open File */
    res = lfs_file_open(efs->fs, &file->file, path, lfs_flags);

    if( res < 0 ) {
        esp_littlefs_free_fd(efs, fd);
        sem_give(efs);
        if(-res != ENOENT)
            ESP_LOGE(TAG, "Failed to open file. Error %s (%d)",
                    esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }

    file->hash = compute_hash(path);
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
    memcpy(file->path, path, path_len);
#endif

#if CONFIG_LITTLEFS_USE_MTIME
    if (!(lfs_flags & LFS_O_RDONLY)) {
        /* If this is being opened as not read-only */
        vfs_littlefs_update_mtime(efs, path);
    }
#endif

    sem_give(efs);
    ESP_LOGD(TAG, "Done opening %s", path);
    return fd;
}

static ssize_t vfs_littlefs_write(void* ctx, int fd, const void * data, size_t size) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    ssize_t res;
    vfs_littlefs_file_t *file = NULL;

    sem_take(efs);
    if((uint32_t)fd > efs->cache_size) {
        sem_give(efs);
        ESP_LOGE(TAG, "FD %d must be <%d.", fd, efs->cache_size);
        errno = -LFS_ERR_BADF;
        return -1;
    }
    file = efs->cache[fd];
    res = lfs_file_write(efs->fs, &file->file, data, size);
    sem_give(efs);

    if(res < 0){
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
        ESP_LOGE(TAG, "Failed to write FD %d; path \"%s\". Error %s (%d)",
                fd, file->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to write FD %d. Error %s (%d)",
                fd, esp_littlefs_errno(res), res);
#endif
        errno = -res;
        return -1;
    }

    return res;
}

static ssize_t vfs_littlefs_read(void* ctx, int fd, void * dst, size_t size) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    ssize_t res;
    vfs_littlefs_file_t *file = NULL;


    sem_take(efs);
    if((uint32_t)fd > efs->cache_size) {
        sem_give(efs);
        ESP_LOGE(TAG, "FD %d must be <%d.", fd, efs->cache_size);
        errno = -LFS_ERR_BADF;
        return -1;
    }
    file = efs->cache[fd];
    res = lfs_file_read(efs->fs, &file->file, dst, size);
    sem_give(efs);

    if(res < 0){
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
        ESP_LOGE(TAG, "Failed to read file \"%s\". Error %s (%d)",
                file->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to read FD %d. Error %s (%d)",
                fd, esp_littlefs_errno(res), res);
#endif
        errno = -res;
        return -1;
    }

    return res;
}

static int vfs_littlefs_close(void* ctx, int fd) {
    // TODO update mtime on close? SPIFFS doesn't do this
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    int res;
    vfs_littlefs_file_t *file = NULL;

    sem_take(efs);
    if((uint32_t)fd > efs->cache_size) {
        sem_give(efs);
        ESP_LOGE(TAG, "FD %d must be <%d.", fd, efs->cache_size);
        errno = -LFS_ERR_BADF;
        return -1;
    }
    file = efs->cache[fd];
    res = lfs_file_close(efs->fs, &file->file);
    if(res < 0){
        sem_give(efs);
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
        ESP_LOGE(TAG, "Failed to close file \"%s\". Error %s (%d)",
                file->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to close Fd %d. Error %s (%d)",
                fd, esp_littlefs_errno(res), res);
#endif
        errno = -res;
        return -1;
    }
    esp_littlefs_free_fd(efs, fd);
    sem_give(efs);
    return 0;
}

static off_t vfs_littlefs_lseek(void* ctx, int fd, off_t offset, int mode) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    lfs_soff_t res;
    vfs_littlefs_file_t *file = NULL;
    int whence;

    switch (mode) {
        case SEEK_SET: whence = LFS_SEEK_SET; break;
        case SEEK_CUR: whence = LFS_SEEK_CUR; break;
        case SEEK_END: whence = LFS_SEEK_END; break;
        default: 
            ESP_LOGE(TAG, "Invalid mode");
            return -1;
    }

    sem_take(efs);
    if((uint32_t)fd > efs->cache_size) {
        sem_give(efs);
        ESP_LOGE(TAG, "FD %d must be <%d.", fd, efs->cache_size);
        errno = -LFS_ERR_BADF;
        return -1;
    }
    file = efs->cache[fd];
    res = lfs_file_seek(efs->fs, &file->file, offset, whence);
    sem_give(efs);

    if(res < 0){
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
        ESP_LOGE(TAG, "Failed to seek file \"%s\" to offset %08x. Error %s (%d)",
                file->path, (unsigned int)offset, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to seek FD %d to offset %08x. Error (%d)",
                fd, (unsigned int)offset, res);
#endif
        errno = -res;
        return -1;
    }

    return res;
}

static int vfs_littlefs_fsync(void* ctx, int fd)
{
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    ssize_t res;
    vfs_littlefs_file_t *file = NULL;



    sem_take(efs);
    if((uint32_t)fd > efs->cache_size) {
        sem_give(efs);
        ESP_LOGE(TAG, "FD %d must be <%d.", fd, efs->cache_size);
        errno = -LFS_ERR_BADF;
        return -1;
    }
    file = efs->cache[fd];
    res = lfs_file_sync(efs->fs, &file->file);
    sem_give(efs);

    if(res < 0){
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
        ESP_LOGE(TAG, "Failed to sync file \"%s\". Error %s (%d)",
                file->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to sync file %d. Error %d", fd, res);
#endif
        errno = -res;
        return -1;
    }

    return 0;
}


#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH
static int vfs_littlefs_fstat(void* ctx, int fd, struct stat * st) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    struct lfs_info info;
    int res;
    vfs_littlefs_file_t *file = NULL;

    memset(st, 0, sizeof(struct stat));
    st->st_blksize = efs->cfg.block_size;

    sem_take(efs);
    if((uint32_t)fd > efs->cache_size) {
        sem_give(efs);
        ESP_LOGE(TAG, "FD must be <%d.", efs->cache_size);
        errno = -LFS_ERR_BADF;
        return -1;
    }
    file = efs->cache[fd];
    res = lfs_stat(efs->fs, file->path, &info);
    if (res < 0) {
        sem_give(efs);
        if(-res != ENOENT)
            ESP_LOGE(TAG, "Failed to stat file \"%s\". Error %s (%d)",
                    file->path, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }

#if CONFIG_LITTLEFS_USE_MTIME  
    st->st_mtime = vfs_littlefs_get_mtime(efs, file->path);
#endif

    sem_give(efs);

    st->st_size = info.size;
    st->st_mode = ((info.type==LFS_TYPE_REG)?S_IFREG:S_IFDIR);
    return 0;
}
#endif

static int vfs_littlefs_stat(void* ctx, const char * path, struct stat * st) {
    assert(path);
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    struct lfs_info info;
    int res;

    memset(st, 0, sizeof(struct stat));
    st->st_blksize = efs->cfg.block_size;

    sem_take(efs);
    res = lfs_stat(efs->fs, path, &info);
    sem_give(efs);
    if (res < 0) {
        /* Not strictly an error, since stat can be used to check
         * if a file exists */
        ESP_LOGI(TAG, "Failed to stat path \"%s\". Error %s (%d)",
                path, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }
    st->st_size = info.size;
#if CONFIG_LITTLEFS_USE_MTIME    
    st->st_mtime = vfs_littlefs_get_mtime(efs, path);
#endif
    st->st_mode = ((info.type==LFS_TYPE_REG)?S_IFREG:S_IFDIR);
    return 0;
}

static int vfs_littlefs_unlink(void* ctx, const char *path) {
#define fail_str_1 "Failed to unlink path \"%s\"."
    assert(path);
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    struct lfs_info info;
    int res;

    sem_take(efs);
    res = lfs_stat(efs->fs, path, &info);
    if (res < 0) {
        sem_give(efs);
        ESP_LOGE(TAG, fail_str_1 " Error %s (%d)",
                path, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }

    if(esp_littlefs_get_fd_by_name(efs, path) >= 0) {
        sem_give(efs);
        ESP_LOGE(TAG, fail_str_1 " Has open FD.", path);
        errno = -res;
        return -1;
    }

    if (info.type == LFS_TYPE_DIR) {
        sem_give(efs);
        ESP_LOGE(TAG, "Cannot unlink a directory.");
        errno = -LFS_ERR_ISDIR;
        return -1;
    }

    res = lfs_remove(efs->fs, path);
    if (res < 0) {
        sem_give(efs);
        ESP_LOGE(TAG, fail_str_1 " Error %s (%d)",
                path, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }

    sem_give(efs);

    return 0;
#undef fail_str_1
}

static int vfs_littlefs_rename(void* ctx, const char *src, const char *dst) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    int res;

    sem_take(efs);

    if(esp_littlefs_get_fd_by_name(efs, src) >= 0){
        sem_give(efs);
        ESP_LOGE(TAG, "Cannot rename; src \"%s\" is open.", src);
        errno = EBUSY;
        return -1;
    }
    else if(esp_littlefs_get_fd_by_name(efs, dst) >= 0){
        sem_give(efs);
        ESP_LOGE(TAG, "Cannot rename; dst \"%s\" is open.", dst);
        errno = EBUSY;
        return -1;
    }

    res = lfs_rename(efs->fs, src, dst);
    sem_give(efs);
    if (res < 0) {
        ESP_LOGE(TAG, "Failed to rename \"%s\" -> \"%s\". Error %s (%d)",
                src, dst, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }

    return 0;
}

static DIR* vfs_littlefs_opendir(void* ctx, const char* name) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    int res;
    vfs_littlefs_dir_t *dir = NULL;

    dir = low_calloc(1, sizeof(vfs_littlefs_dir_t));
    if( dir == NULL ) {
        ESP_LOGE(TAG, "dir struct could not be malloced");
        goto exit;
    }

    dir->path = strdup(name);
    if(dir->path == NULL){
        ESP_LOGE(TAG, "dir path name could not be malloced");
        goto exit;
    }

    sem_take(efs);
    res = lfs_dir_open(efs->fs, &dir->d, dir->path);
    sem_give(efs);
    if (res < 0) {
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH        
        ESP_LOGE(TAG, "Failed to opendir \"%s\". Error %s (%d)",
                dir->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to opendir \"%s\". Error %d", dir->path, res);
#endif
        goto exit;
    }

    return (DIR *)dir;

exit:
    esp_littlefs_dir_free(dir);
    return NULL;
}

static int vfs_littlefs_closedir(void* ctx, DIR* pdir) {
    assert(pdir);
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    vfs_littlefs_dir_t * dir = (vfs_littlefs_dir_t *) pdir;
    int res;

    sem_take(efs);
    res = lfs_dir_close(efs->fs, &dir->d);
    sem_give(efs);
    if (res < 0) {
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH        
        ESP_LOGE(TAG, "Failed to closedir \"%s\". Error %s (%d)",
                dir->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to closedir \"%s\". Error %d", dir->path, res);
#endif
        errno = -res;
        return -1;
    }

    esp_littlefs_dir_free(dir);
    return 0;
}

static struct dirent* vfs_littlefs_readdir(void* ctx, DIR* pdir) {
    assert(pdir);
    vfs_littlefs_dir_t * dir = (vfs_littlefs_dir_t *) pdir;
    int res;
    struct dirent* out_dirent;

    res = vfs_littlefs_readdir_r(ctx, pdir, &dir->e, &out_dirent);
    if (res != 0) return NULL;
    return out_dirent;
}

static int vfs_littlefs_readdir_r(void* ctx, DIR* pdir,
        struct dirent* entry, struct dirent** out_dirent) {
    assert(pdir);
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    vfs_littlefs_dir_t * dir = (vfs_littlefs_dir_t *) pdir;
    int res;
    struct lfs_info info = { 0 };

    sem_take(efs);
    do{ /* Read until we get a real object name */
        res = lfs_dir_read(efs->fs, &dir->d, &info);
    }while( res>0 && (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0));
    sem_give(efs);
    if (res < 0) {
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH 
        ESP_LOGE(TAG, "Failed to readdir \"%s\". Error %s (%d)",
                dir->path, esp_littlefs_errno(res), res);
#else
        ESP_LOGE(TAG, "Failed to readdir \"%s\". Error %d", dir->path, res);
#endif
        errno = -res;
        return -1;
    }

    if(info.type == LFS_TYPE_REG) {
        ESP_LOGD(TAG, "readdir a file of size %d named \"%s\"",
                info.size, info.name);
    }
    else {
        ESP_LOGD(TAG, "readdir a dir named \"%s\"", info.name);
    }

    if(res == 0) {
        /* End of Objs */
        ESP_LOGD(TAG, "Reached the end of the directory.");
        *out_dirent = NULL;
    }
    else {
        entry->d_ino = 0;
        entry->d_type = info.type == LFS_TYPE_REG ? DT_REG : DT_DIR;
        strncpy(entry->d_name, info.name, sizeof(entry->d_name));
        *out_dirent = entry;
    }
    dir->offset++;

    return 0;
}

static long vfs_littlefs_telldir(void* ctx, DIR* pdir) {
    assert(pdir);
    vfs_littlefs_dir_t * dir = (vfs_littlefs_dir_t *) pdir;
    return dir->offset;
}

static void vfs_littlefs_seekdir(void* ctx, DIR* pdir, long offset) {
    assert(pdir);
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    vfs_littlefs_dir_t * dir = (vfs_littlefs_dir_t *) pdir;
    int res;

    if (offset < dir->offset) {
        /* close and re-open dir to rewind to beginning */
        sem_take(efs);
        res = lfs_dir_rewind(efs->fs, &dir->d);
        sem_give(efs);
        if (res < 0) {
            ESP_LOGE(TAG, "Failed to rewind dir \"%s\". Error %s (%d)",
                    dir->path, esp_littlefs_errno(res), res);
            return;
        }
        dir->offset = 0;
    }

    while(dir->offset < offset){
        struct dirent *out_dirent;
        res = vfs_littlefs_readdir_r(ctx, pdir, &dir->e, &out_dirent);
        if( res != 0 ){
            ESP_LOGE(TAG, "Error readdir_r");
            return;
        }
    }
}

static int vfs_littlefs_mkdir(void* ctx, const char* name, mode_t mode) {
    /* Note: mode is currently unused */
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    int res;
    ESP_LOGD(TAG, "mkdir \"%s\"", name);

    sem_take(efs);
    res = lfs_mkdir(efs->fs, name);
    sem_give(efs);
    if (res < 0) {
        if(-res != EEXIST)
            ESP_LOGE(TAG, "Failed to mkdir \"%s\". Error %s (%d)",
                    name, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }
    return 0;
}

static int vfs_littlefs_rmdir(void* ctx, const char* name) {
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    struct lfs_info info;
    int res;

    /* Error Checking */
    sem_take(efs);
    res = lfs_stat(efs->fs, name, &info);
    if (res < 0) {
        sem_give(efs);
        ESP_LOGE(TAG, "\"%s\" doesn't exist.", name);
        errno = -res;
        return -1;
    }

    if (info.type != LFS_TYPE_DIR) {
        sem_give(efs);
        ESP_LOGE(TAG, "\"%s\" is not a directory.", name);
        errno = ENOTDIR;
        return -1;
    }

    /* Unlink the dir */
    res = lfs_remove(efs->fs, name);
    sem_give(efs);
    if ( res < 0) {
        ESP_LOGE(TAG, "Failed to unlink path \"%s\". Error %s (%d)",
                name, esp_littlefs_errno(res), res);
        errno = -res;
        return -1;
    }

    return 0;
}

#if CONFIG_LITTLEFS_USE_MTIME
/**
 * Sets the mtime attr to t.
 */
static int vfs_littlefs_update_mtime_value(esp_littlefs_t *efs, const char *path, time_t t)
{
    int res;
    sem_take(efs);
    res = lfs_setattr(efs->fs, path, LITTLEFS_ATTR_MTIME,
            &t, sizeof(t));
    sem_give(efs);
    if( res < 0 ) {
        ESP_LOGE(TAG, "Failed to update mtime (%d)", res);
        errno = -res;
        return -1;
    }

    return 0;
}

/**
 * Sets the mtime attr to an appropriate value
 */
static void vfs_littlefs_update_mtime(esp_littlefs_t *efs, const char *path)
{
    vfs_littlefs_utime(efs, path, NULL);
}


static int vfs_littlefs_utime(void *ctx, const char *path, const struct utimbuf *times)
{
    esp_littlefs_t * efs = (esp_littlefs_t *)ctx;
    time_t t;

    assert(path);

    if (times) {
        t = times->modtime;
    } else {
#if CONFIG_LITTLEFS_MTIME_USE_SECONDS
        // use current time
        t = time(NULL);
#elif CONFIG_LITTLEFS_MTIME_USE_NONCE
        assert( sizeof(time_t) == 4 );
        t = vfs_littlefs_get_mtime(efs, path);
        if( 0 == t ) t = esp_random();
        else t += 1;

        if( 0 == t ) t = 1;
#else
#error "Invalid MTIME configuration"
#endif
    }

    return vfs_littlefs_update_mtime_value(efs, path, t);
}

static time_t vfs_littlefs_get_mtime(esp_littlefs_t *efs, const char *path)
{
    time_t t = 0;
    int size;
    sem_take(efs);
    size = lfs_getattr(efs->fs, path, LITTLEFS_ATTR_MTIME,
            &t, sizeof(t));
    sem_give(efs);
    if( size < 0 ) {
#ifndef CONFIG_LITTLEFS_USE_ONLY_HASH        
        ESP_LOGI(TAG, "Failed to get mtime attribute %s (%d)",
                esp_littlefs_errno(size), size);
#else
        ESP_LOGI(TAG, "Failed to get mtime attribute %d", size);
#endif
        errno = -size;
        return -1;
    }
    return size;
}
#endif //CONFIG_LITTLEFS_USE_MTIME
