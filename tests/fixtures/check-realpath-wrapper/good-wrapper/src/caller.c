/* The correct call. MUST NOT match: the wrapper's identifier ends with the
 * same seven letters as the bare libc function, so this fixture pins the
 * prefix guard. Remove the [^_a-zA-Z0-9] class from the pattern and this
 * case starts failing on correct code — in the real tree, in six files. */
char *resolve(hu_allocator_t *alloc, const char *path) {
    return hu_platform_realpath(alloc, path);
}
