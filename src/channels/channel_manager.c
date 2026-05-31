#include "human/channel_manager.h"
#include <string.h>

hu_error_t hu_channel_manager_init(hu_channel_manager_t *mgr, hu_allocator_t *alloc) {
    if (!mgr || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    return HU_OK;
}

void hu_channel_manager_deinit(hu_channel_manager_t *mgr) {
    if (!mgr)
        return;
    hu_channel_manager_stop_all(mgr);
    memset(mgr, 0, sizeof(*mgr));
}

void hu_channel_manager_set_bus(hu_channel_manager_t *mgr, hu_bus_t *bus) {
    if (mgr)
        mgr->event_bus = bus;
}

hu_error_t hu_channel_manager_register(hu_channel_manager_t *mgr, const char *name,
                                       const char *account_id, const hu_channel_t *channel,
                                       hu_channel_listener_type_t listener_type) {
    if (!mgr || !name || !channel)
        return HU_ERR_INVALID_ARGUMENT;
    if (mgr->count >= HU_CHANNEL_MANAGER_MAX)
        return HU_ERR_ALREADY_EXISTS;
    hu_channel_entry_t *e = &mgr->entries[mgr->count++];
    e->name = name;
    e->account_id = account_id ? account_id : "default";
    e->channel = *channel;
    e->listener_type = listener_type;
    return HU_OK;
}

hu_error_t hu_channel_manager_start_all(hu_channel_manager_t *mgr) {
    if (!mgr)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t first_err = HU_OK;
    for (size_t i = 0; i < mgr->count; i++) {
        hu_channel_entry_t *e = &mgr->entries[i];
        if (e->listener_type == HU_CHANNEL_LISTENER_NONE)
            continue;
        if (e->channel.vtable && e->channel.vtable->start) {
            hu_error_t err = e->channel.vtable->start(e->channel.ctx);
            if (err != HU_OK && first_err == HU_OK)
                first_err = err;
        }
    }
    return first_err;
}

void hu_channel_manager_stop_all(hu_channel_manager_t *mgr) {
    if (!mgr)
        return;
    for (size_t i = 0; i < mgr->count; i++) {
        hu_channel_entry_t *e = &mgr->entries[i];
        if (e->channel.vtable && e->channel.vtable->stop) {
            e->channel.vtable->stop(e->channel.ctx);
        }
    }
}

const hu_channel_entry_t *hu_channel_manager_entries(const hu_channel_manager_t *mgr,
                                                     size_t *out_count) {
    if (!mgr) {
        if (out_count)
            *out_count = 0;
        return NULL;
    }
    if (out_count)
        *out_count = mgr->count;
    return mgr->entries;
}

size_t hu_channel_manager_count(const hu_channel_manager_t *mgr) {
    return mgr ? mgr->count : 0;
}
