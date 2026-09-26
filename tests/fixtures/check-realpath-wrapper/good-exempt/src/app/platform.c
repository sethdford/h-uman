/* The one legal raw call: the wrapper's own implementation. */
char *hu_platform_realpath(hu_allocator_t *alloc, const char *path) {
    (void)alloc;
    return realpath(path, NULL);
}
