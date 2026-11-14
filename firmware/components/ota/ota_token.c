#include "ota_token.h"

#include "ota_proto.h"

bool ota_token_text_valid(char const *text) {
    for (int i = 0; i < OTA_TOKEN_LEN; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}
