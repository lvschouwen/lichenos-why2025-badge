#include "meshcore_app_record.h"

#include "mesh_advert_cfg.h"

// Kept over a factory reset: the last advert, so the advert interval (#52) holds and a reset loop never floods
char const *const meshcore_keep_keys[] = {ADV_KEY_LAST, NULL};

// The notification switches may go in the SD backup (#169); what the badge transmits stays out of it
sb_key_t const meshcore_backup_keys[MESHCORE_BACKUP_KEYS] = {
    {"a." MESHCORE_APP_ID, "n_dm", "Notify for direct messages", SB_U8, 0, 1, 1, NULL},
    {"a." MESHCORE_APP_ID, "n_fail", "Notify for failed sends", SB_U8, 0, 1, 1, NULL},
    {"a." MESHCORE_APP_ID, "n_ment", "Notify for mentions", SB_U8, 0, 1, 1, NULL},
};
