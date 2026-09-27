/* The classic spelling. Caught by the pre-2026-09-21 pattern too. */
char *resolve(const char *path) {
    return realpath(path, NULL);
}
