#include "seaclaw/channel_manager.h"
#include <string.h>

sc_error_t sc_channel_manager_init(sc_channel_manager_t *mgr,
                                   sc_allocator_t *alloc) {
    if (!mgr || !alloc) return SC_ERR_INVALID_ARGUMENT;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    return SC_OK;
}

void sc_channel_manager_deinit(sc_channel_manager_t *mgr) {
    if (!mgr) return;
    sc_channel_manager_stop_all(mgr);
    memset(mgr, 0, sizeof(*mgr));
}

void sc_channel_manager_set_bus(sc_channel_manager_t *mgr, sc_bus_t *bus) {
    if (mgr) mgr->event_bus = bus;
}

sc_error_t sc_channel_manager_register(sc_channel_manager_t *mgr,
                                        const char *name,
                                        const char *account_id,
                                        const sc_channel_t *channel,
                                        sc_channel_listener_type_t listener_type) {
    if (!mgr || !name || !channel) return SC_ERR_INVALID_ARGUMENT;
    if (mgr->count >= SC_CHANNEL_MANAGER_MAX) return SC_ERR_ALREADY_EXISTS;
    sc_channel_entry_t *e = &mgr->entries[mgr->count++];
    e->name = name;
    e->account_id = account_id ? account_id : "default";
    e->channel = *channel;
    e->listener_type = listener_type;
    return SC_OK;
}

sc_error_t sc_channel_manager_start_all(sc_channel_manager_t *mgr) {
    if (!mgr) return SC_ERR_INVALID_ARGUMENT;
    sc_error_t first_err = SC_OK;
    for (size_t i = 0; i < mgr->count; i++) {
        sc_channel_entry_t *e = &mgr->entries[i];
        if (e->listener_type == SC_CHANNEL_LISTENER_NONE) continue;
        if (e->channel.vtable && e->channel.vtable->start) {
            sc_error_t err = e->channel.vtable->start(e->channel.ctx);
            if (err != SC_OK && first_err == SC_OK) first_err = err;
        }
    }
    return first_err;
}

void sc_channel_manager_stop_all(sc_channel_manager_t *mgr) {
    if (!mgr) return;
    for (size_t i = 0; i < mgr->count; i++) {
        sc_channel_entry_t *e = &mgr->entries[i];
        if (e->channel.vtable && e->channel.vtable->stop) {
            e->channel.vtable->stop(e->channel.ctx);
        }
    }
}

const sc_channel_entry_t *sc_channel_manager_entries(const sc_channel_manager_t *mgr,
                                                      size_t *out_count) {
    if (!mgr) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) *out_count = mgr->count;
    return mgr->entries;
}

size_t sc_channel_manager_count(const sc_channel_manager_t *mgr) {
    return mgr ? mgr->count : 0;
}
