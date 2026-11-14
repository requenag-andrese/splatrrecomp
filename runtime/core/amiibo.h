// sr-recomp: the application area, events and nickname additions are marked below (amiibo.c says why).
// Amiibo loader and state management subsystem (540-byte NTAG215 binary dumps).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMIIBO_DUMP_SIZE 540

// Device state corresponding to nn::nfp / nn::nfc DeviceState
typedef enum {
    AMIIBO_DEV_STATE_INITIALIZED = 0,
    AMIIBO_DEV_STATE_SEARCHING   = 1,
    AMIIBO_DEV_STATE_TAG_FOUND   = 2,
    AMIIBO_DEV_STATE_TAG_REMOVED = 3,
    AMIIBO_DEV_STATE_TAG_MOUNTED = 4,
    AMIIBO_DEV_STATE_UNAVAILABLE = 5
} AmiiboDeviceState;

// Amiibo identification info
typedef struct {
    uint8_t uuid[10];
    uint8_t uuid_len;
    uint8_t character_id[3];
    uint8_t series_id;
    uint16_t numbering_id;
    uint8_t nfp_type;
    uint8_t version;
    char path[512];
    char filename[128];
} AmiiboInfo;

// Subsystem lifecycle
void amiibo_init(void);

// Hardware / device emulation status
void amiibo_set_device_present(bool present);
bool amiibo_is_device_present(void);

// Tag detection & lifecycle
void amiibo_start_detection(void);
void amiibo_stop_detection(void);
bool amiibo_is_detecting(void);

// Tag mounting
bool amiibo_mount(void);
void amiibo_unmount(void);
bool amiibo_is_mounted(void);

// State query
AmiiboDeviceState amiibo_get_device_state(void);
// The state the menu shows: searching also within a second after a stop, since the game restarts detection many times
// a second while it waits for a tag. The game reads amiibo_get_device_state.
AmiiboDeviceState amiibo_get_reader_state(void);

// File loading & triggering actions
bool amiibo_load_file(const char *path);
bool amiibo_scan_and_tap(const char *path);
void amiibo_tap_tag(void);
void amiibo_remove_tag(void);

// Timer tick called during menu / frame updates
void amiibo_tick(void);

// Options
void amiibo_set_auto_remove(bool enable);
bool amiibo_get_auto_remove(void);
void amiibo_set_auto_remove_delay_s(float seconds);
float amiibo_get_auto_remove_delay_s(void);

// Tag inspection
bool amiibo_has_tag(void);
const AmiiboInfo *amiibo_get_current_info(void);
const uint8_t *amiibo_get_raw_data(void);

// sr-recomp: the game's figure placed / removed events, signalled on a tap and a removal (0 keeps the one set)
void amiibo_set_events(uint64_t activate, uint64_t deactivate);
void amiibo_nickname(char *out, size_t n);   // what the register info names the figure

// sr-recomp: the simulated application area (amiibo.c: a file of the profile per figure, not the encrypted dump's)
typedef enum {
    AMIIBO_APP_OK, AMIIBO_APP_NO_TAG, AMIIBO_APP_NONE, AMIIBO_APP_OTHER_ID, AMIIBO_APP_NOT_OPEN, AMIIBO_APP_BAD_SIZE,
    AMIIBO_APP_EXISTS, AMIIBO_APP_WRITE_FAILED
} AmiiboAppResult;
AmiiboAppResult amiibo_app_open(uint32_t access_id);
AmiiboAppResult amiibo_app_get(void *out, uint64_t max, uint64_t *size);
AmiiboAppResult amiibo_app_set(const void *data, uint64_t size);
AmiiboAppResult amiibo_app_create(uint32_t access_id, const void *data, uint64_t size, int recreate);
AmiiboAppResult amiibo_app_flush(void);
void amiibo_app_erase(void);

// Horizon OS buffers extraction (called by NFP IPC/override services)
void amiibo_get_tag_info(void *out_tag_info);
void amiibo_get_model_info(void *out_model_info);
void amiibo_get_common_info(void *out_common_info);
void amiibo_get_register_info(void *out_register_info);
int amiibo_get_application_area(void *out_buf, uint32_t max_size);

#ifdef __cplusplus
}
#endif
