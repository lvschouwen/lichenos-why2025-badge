// Plain libc behind the why_* file calls that tomlc17 makes: host tests that parse TOML, and the lean firmware (#61)
#include "why_io.h"

FILE *why_fopen(char const *restrict pathname, char const *restrict mode) {
    return fopen(pathname, mode);
}

int why_fclose(FILE *stream) {
    return fclose(stream);
}

size_t why_fread(void *ptr, size_t size, size_t nmemb, FILE *restrict stream) {
    return fread(ptr, size, nmemb, stream);
}

int why_feof(FILE *stream) {
    return feof(stream);
}

int why_ferror(FILE *stream) {
    return ferror(stream);
}
