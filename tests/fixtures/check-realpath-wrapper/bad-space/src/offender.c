/* A space before the paren. Legal C, and INVISIBLE to the old
 * `[^_a-zA-Z]realpath\(` pattern — this fixture is the regression. */
char *resolve(const char *path) {
    return realpath(path, NULL);
}
