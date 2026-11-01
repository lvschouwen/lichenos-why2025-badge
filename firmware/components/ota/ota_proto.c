#include "ota_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define LINE_MAX_LEN       255
#define CONTENT_LENGTH_MAX 12 // digits; far beyond any partition

static bool is_hex_string(char const *s, size_t want_len) {
    if (strlen(s) != want_len) {
        return false;
    }
    for (; *s; s++) {
        if (!isxdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static void to_lower(char *s) {
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

size_t ota_head_length(char const *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

// A decimal size of at most CONTENT_LENGTH_MAX digits; anything else (signs, junk) is refused
static bool parse_size(char const *value, size_t vlen, long long *out) {
    if (vlen == 0 || vlen > CONTENT_LENGTH_MAX) {
        return false;
    }
    long long n = 0;
    for (char const *p = value; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        n = n * 10 + (*p - '0');
    }
    *out = n;
    return true;
}

static int parse_request_line(char *line, ota_request_t *req) {
    char *method  = strtok(line, " ");
    char *target  = strtok(NULL, " ");
    char *version = strtok(NULL, " ");
    if (!method || !target || !version || strtok(NULL, " ") || strncmp(version, "HTTP/1.", 7) != 0) {
        return 400;
    }

    static struct {
        char const *target;
        char const *method;
        ota_route_t route;
    } const routes[] = {
        {"/info", "GET", OTA_ROUTE_INFO},
        {"/firmware", "PUT", OTA_ROUTE_FIRMWARE},
        {"/file", "PUT", OTA_ROUTE_FILE},
        {"/reboot", "POST", OTA_ROUTE_REBOOT},
        {"/prepare", "POST", OTA_ROUTE_PREPARE},
        {"/assets", "PUT", OTA_ROUTE_ASSETS},
        {"/file", "DELETE", OTA_ROUTE_DELETE},
        {"/screenshot", "GET", OTA_ROUTE_SCREENSHOT},
        {"/log", "GET", OTA_ROUTE_LOG},
        {"/console", "POST", OTA_ROUTE_CONSOLE},
    };
    // Only /log takes a query: "?since=<seq>" (digits). Every other target with a '?' stays unknown.
    char *query = strchr(target, '?');
    if (query && strncmp(target, "/log?", 5) == 0) {
        *query = '\0';
        if (strncmp(query + 1, "since=", 6) != 0 || !parse_size(query + 7, strlen(query + 7), &req->since)) {
            return 400;
        }
    }
    bool known_target = false;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (strcmp(target, routes[i].target) == 0) {
            known_target = true;
            if (strcmp(method, routes[i].method) == 0) {
                req->route = routes[i].route;
                return 0;
            }
        }
    }
    return known_target ? 405 : 404;
}

static int parse_header(char *line, ota_request_t *req) {
    char *colon = strchr(line, ':');
    if (!colon || colon == line) {
        return 400;
    }
    *colon      = '\0';
    char *name  = line;
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    size_t vlen = strlen(value);
    while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
        value[--vlen] = '\0';
    }

    if (strcasecmp(name, "Content-Length") == 0) {
        if (!parse_size(value, vlen, &req->content_length)) {
            return 400;
        }
    } else if (strcasecmp(name, "X-Offset") == 0) {
        if (!parse_size(value, vlen, &req->offset)) {
            return 400;
        }
        req->has_offset = true;
    } else if (strcasecmp(name, "X-Total-Size") == 0) {
        if (!parse_size(value, vlen, &req->total_size)) {
            return 400;
        }
    } else if (strcasecmp(name, "X-File-SHA256") == 0) {
        if (!is_hex_string(value, OTA_SHA256_HEX)) {
            return 400;
        }
        strcpy(req->file_sha256, value);
        to_lower(req->file_sha256);
        req->has_file_sha256 = true;
    } else if (strcasecmp(name, "Authorization") == 0) {
        if (strncasecmp(value, "Bearer ", 7) != 0 || !is_hex_string(value + 7, OTA_TOKEN_LEN)) {
            return 400;
        }
        strcpy(req->token, value + 7);
        to_lower(req->token);
        req->has_token = true;
    } else if (strcasecmp(name, "X-Content-SHA256") == 0) {
        if (!is_hex_string(value, OTA_SHA256_HEX)) {
            return 400;
        }
        strcpy(req->sha256, value);
        to_lower(req->sha256);
        req->has_sha256 = true;
    } else if (strcasecmp(name, "X-Path") == 0) {
        if (vlen == 0 || vlen > OTA_PATH_MAX) {
            return 400;
        }
        strcpy(req->path, value);
        req->has_path = true;
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
        return 400; // only Content-Length bodies
    } else if (strcasecmp(name, "Content-Encoding") == 0) {
        if (strcasecmp(value, "deflate") == 0) {
            req->deflate = true;
        } else if (strcasecmp(value, "identity") != 0) {
            return 415;
        }
    } else if (strcasecmp(name, "Expect") == 0) {
        req->expect_continue = strcasecmp(value, "100-continue") == 0;
    }
    return 0;
}

int ota_parse_request(char const *head, size_t len, ota_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
    req->total_size     = -1;

    char   line[LINE_MAX_LEN + 1];
    bool   first = true;
    size_t pos   = 0;
    while (pos < len) {
        char const *end = NULL;
        for (size_t i = pos; i + 1 < len; i++) {
            if (head[i] == '\r' && head[i + 1] == '\n') {
                end = head + i;
                break;
            }
        }
        if (!end) {
            return 400;
        }
        size_t line_len = (size_t)(end - (head + pos));
        if (line_len == 0) {
            return first ? 400 : 0; // the blank line ends the head
        }
        if (line_len > LINE_MAX_LEN) {
            return 400;
        }
        memcpy(line, head + pos, line_len);
        line[line_len] = '\0';
        pos            += line_len + 2;

        int status = first ? parse_request_line(line, req) : parse_header(line, req);
        if (status) {
            return status;
        }
        first = false;
    }
    return 400;
}

static bool name_char(char c, bool dot_ok) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || (dot_ok && c == '.');
}

bool ota_path_allowed(char const *path) {
    if (strlen(path) > OTA_PATH_MAX) {
        return false;
    }
    char const *p;
    if (strncmp(path, "FLASH0:", 7) == 0) {
        p = path + 7;
    } else if (strncmp(path, "SD0:", 4) == 0) {
        p = path + 4;
    } else {
        return false;
    }

    if (*p == '[') {
        p++;
        bool part_empty = true;
        for (; *p != ']'; p++) {
            if (*p == '.') {
                if (part_empty) {
                    return false;
                }
                part_empty = true;
            } else if (name_char(*p, false)) {
                part_empty = false;
            } else {
                return false; // also the end of the string without a ']'
            }
        }
        if (part_empty) {
            return false;
        }
        p++;
    }

    if (*p == '\0' || *p == '.' || strncmp(p, OTA_TEMP_NAME, strlen(OTA_TEMP_NAME)) == 0) {
        return false;
    }
    for (; *p; p++) {
        if (!name_char(*p, true)) {
            return false;
        }
    }
    return true;
}

bool ota_temp_path(char const *path, char *out, size_t cap) {
    char const *dir_end = strchr(path, ']');
    if (!dir_end) {
        dir_end = strchr(path, ':');
    }
    if (!dir_end) {
        return false;
    }
    int n = snprintf(out, cap, "%.*s%s", (int)(dir_end - path + 1), path, OTA_TEMP_NAME);
    return n > 0 && (size_t)n < cap;
}

bool ota_token_equal(char const *a, char const *b) {
    if (strlen(a) != OTA_TOKEN_LEN || strlen(b) != OTA_TOKEN_LEN) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < OTA_TOKEN_LEN; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

void ota_hex(uint8_t const *bytes, size_t len, char *out) {
    static char const digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0xF];
    }
    out[len * 2] = '\0';
}

bool ota_route_needs_token(ota_route_t route) {
    (void)route;
    return true;
}
