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

// A decimal size of at most CONTENT_LENGTH_MAX digits (so it cannot overflow); anything else (signs, junk) is refused
static bool parse_size(char const *value, size_t vlen, long long *out) {
    if (vlen == 0 || vlen > CONTENT_LENGTH_MAX) {
        return false;
    }
    long long n = 0;
    for (size_t i = 0; i < vlen; i++) {
        if (!isdigit((unsigned char)value[i])) {
            return false;
        }
        n = n * 10 + (value[i] - '0');
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
        {"/crashes", "GET", OTA_ROUTE_CRASHES},
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

// One header's value, already trimmed: each handler checks it and stores it, or returns the status to refuse with
typedef int (*header_fn)(char *value, size_t vlen, ota_request_t *req);

static int size_into(char const *value, size_t vlen, long long *out) {
    return parse_size(value, vlen, out) ? 0 : 400;
}

static int hex_into(char const *value, char *out) {
    if (!is_hex_string(value, OTA_SHA256_HEX)) {
        return 400;
    }
    memcpy(out, value, OTA_SHA256_HEX + 1); // is_hex_string checked the length, NUL included
    to_lower(out);
    return 0;
}

static int h_content_length(char *value, size_t vlen, ota_request_t *req) {
    return size_into(value, vlen, &req->content_length);
}

static int h_offset(char *value, size_t vlen, ota_request_t *req) {
    int status      = size_into(value, vlen, &req->offset);
    req->has_offset = status == 0;
    return status;
}

static int h_total_size(char *value, size_t vlen, ota_request_t *req) {
    return size_into(value, vlen, &req->total_size);
}

static int h_piece_size(char *value, size_t vlen, ota_request_t *req) {
    return size_into(value, vlen, &req->piece_size);
}

static int h_file_sha256(char *value, size_t vlen, ota_request_t *req) {
    (void)vlen;
    int status           = hex_into(value, req->file_sha256);
    req->has_file_sha256 = status == 0;
    return status;
}

static int h_sha256(char *value, size_t vlen, ota_request_t *req) {
    (void)vlen;
    int status      = hex_into(value, req->sha256);
    req->has_sha256 = status == 0;
    return status;
}

static int h_authorization(char *value, size_t vlen, ota_request_t *req) {
    (void)vlen;
    if (strncasecmp(value, "Bearer ", 7) != 0 || !is_hex_string(value + 7, OTA_TOKEN_LEN)) {
        return 400;
    }
    memcpy(req->token, value + 7, OTA_TOKEN_LEN + 1);
    to_lower(req->token);
    req->has_token = true;
    return 0;
}

static int h_path(char *value, size_t vlen, ota_request_t *req) {
    if (vlen == 0 || vlen > OTA_PATH_MAX) {
        return 400;
    }
    memcpy(req->path, value, vlen + 1);
    req->has_path = true;
    return 0;
}

static int h_transfer_encoding(char *value, size_t vlen, ota_request_t *req) {
    (void)value, (void)vlen, (void)req;
    return 400; // only Content-Length bodies
}

static int h_content_encoding(char *value, size_t vlen, ota_request_t *req) {
    (void)vlen;
    if (strcasecmp(value, "deflate") == 0) {
        req->deflate = true;
        return 0;
    }
    return strcasecmp(value, "identity") == 0 ? 0 : 415;
}

static int h_expect(char *value, size_t vlen, ota_request_t *req) {
    (void)vlen;
    req->expect_continue = strcasecmp(value, "100-continue") == 0;
    return 0;
}

// The headers the receiver reads; any other is ignored. Each may come once (#211): a second copy is refused rather
// than silently winning, so a request can't say two things about its own length, place or token.
static struct {
    char const *name;
    header_fn   fn;
} const headers[] = {
    {"Content-Length", h_content_length},
    {"X-Offset", h_offset},
    {"X-Total-Size", h_total_size},
    {"X-Piece-Size", h_piece_size},
    {"X-File-SHA256", h_file_sha256},
    {"Authorization", h_authorization},
    {"X-Content-SHA256", h_sha256},
    {"X-Path", h_path},
    {"Transfer-Encoding", h_transfer_encoding},
    {"Content-Encoding", h_content_encoding},
    {"Expect", h_expect},
};
_Static_assert(sizeof(headers) / sizeof(headers[0]) <= 32, "one bit of the seen mask per header");

// Split "Name: value" in place into the name and the value without its surrounding blanks; false without a name
static bool split_header(char *line, char **name, char **value, size_t *vlen) {
    char *colon = strchr(line, ':');
    if (!colon || colon == line) {
        return false;
    }
    *colon  = '\0';
    *name   = line;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    size_t n = strlen(v);
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) {
        v[--n] = '\0';
    }
    *value = v;
    *vlen  = n;
    return true;
}

static int parse_header(char *line, ota_request_t *req, uint32_t *seen) {
    char  *name;
    char  *value;
    size_t vlen;
    if (!split_header(line, &name, &value, &vlen)) {
        return 400;
    }
    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
        if (strcasecmp(name, headers[i].name) == 0) {
            if (*seen & (1u << i)) {
                return 400;
            }
            *seen |= 1u << i;
            return headers[i].fn(value, vlen, req);
        }
    }
    return 0;
}

// The next CRLF-terminated line of head from pos into line (NUL-terminated). Returns its length, or -1 when there is
// no CRLF, the line is too long or it holds a NUL (#211: a NUL used to cut a value short without anyone noticing).
static long next_line(char const *head, size_t len, size_t pos, char line[LINE_MAX_LEN + 1]) {
    for (size_t i = pos; i + 1 < len; i++) {
        if (head[i] == '\r' && head[i + 1] == '\n') {
            size_t line_len = i - pos;
            if (line_len > LINE_MAX_LEN || memchr(head + pos, '\0', line_len)) {
                return -1;
            }
            memcpy(line, head + pos, line_len);
            line[line_len] = '\0';
            return (long)line_len;
        }
    }
    return -1;
}

int ota_parse_request(char const *head, size_t len, ota_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
    req->total_size     = -1;
    req->piece_size     = -1;

    char     line[LINE_MAX_LEN + 1];
    uint32_t seen = 0;
    for (size_t pos = 0; pos < len;) {
        long line_len = next_line(head, len, pos, line);
        if (line_len < 0) {
            return 400;
        }
        if (line_len == 0) {
            return pos == 0 ? 400 : 0; // the blank line ends the head
        }
        int status = pos == 0 ? parse_request_line(line, req) : parse_header(line, req, &seen);
        if (status) {
            return status;
        }
        pos += (size_t)line_len + 2;
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

bool ota_token_kept(bool from_file, bool from_nvs, bool wrote_file, bool wrote_nvs) {
    return from_file || from_nvs || wrote_file || wrote_nvs;
}

bool ota_server_listens(bool setting_on, bool token_kept) {
    return setting_on && token_kept;
}

#define LFS_CTZ_BYTES      8 // a LittleFS data block's list pointers, on average (#300)
#define LFS_MARGIN_BLOCKS  4 // metadata pairs the write and the swap may need (a directory commit, a compaction)

bool ota_file_fits(uint64_t free_bytes, uint64_t size, uint32_t block) {
    if (block <= LFS_CTZ_BYTES) {
        return block == 0 && size < UINT64_MAX && free_bytes >= size + 1;
    }
    uint64_t payload = block - LFS_CTZ_BYTES;
    uint64_t blocks  = size / payload + (size % payload != 0) + LFS_MARGIN_BLOCKS;
    return blocks <= UINT64_MAX / block && free_bytes >= blocks * block;
}

void ota_hex(uint8_t const *bytes, size_t len, char *out) {
    static char const digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0xF];
    }
    out[len * 2] = '\0';
}

bool ota_route_stops_apps(ota_route_t route) {
    return route == OTA_ROUTE_PREPARE || route == OTA_ROUTE_FIRMWARE || route == OTA_ROUTE_ASSETS;
}

bool ota_route_needs_token(ota_route_t route) {
    (void)route;
    return true;
}
