/* Outside src/. Invisible to the gate until the scan widened to
 * src apps sdk tools on 2026-09-21. */
char *resolve(const char *path) {
    return realpath(path, NULL);
}
