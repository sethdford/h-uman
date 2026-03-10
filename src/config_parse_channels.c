#include "config_internal.h"
#include "config_parse_internal.h"
#include "seaclaw/config.h"
#include "seaclaw/core/string.h"
#include <string.h>

static void parse_email_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    sc_email_channel_config_t *e = &cfg->channels.email;
    const char *s;
    s = sc_json_get_string(obj, "smtp_host");
    if (s) {
        if (e->smtp_host)
            a->free(a->ctx, e->smtp_host, strlen(e->smtp_host) + 1);
        e->smtp_host = sc_strdup(a, s);
    }
    double port = sc_json_get_number(obj, "smtp_port", e->smtp_port);
    if (port >= 1 && port <= 65535)
        e->smtp_port = (uint16_t)port;
    s = sc_json_get_string(obj, "from_address");
    if (s) {
        if (e->from_address)
            a->free(a->ctx, e->from_address, strlen(e->from_address) + 1);
        e->from_address = sc_strdup(a, s);
    }
    s = sc_json_get_string(obj, "smtp_user");
    if (s) {
        if (e->smtp_user)
            a->free(a->ctx, e->smtp_user, strlen(e->smtp_user) + 1);
        e->smtp_user = sc_strdup(a, s);
    }
    s = sc_json_get_string(obj, "smtp_pass");
    if (s) {
        if (e->smtp_pass)
            a->free(a->ctx, e->smtp_pass, strlen(e->smtp_pass) + 1);
        e->smtp_pass = sc_strdup(a, s);
    }
    s = sc_json_get_string(obj, "imap_host");
    if (s) {
        if (e->imap_host)
            a->free(a->ctx, e->imap_host, strlen(e->imap_host) + 1);
        e->imap_host = sc_strdup(a, s);
    }
    port = sc_json_get_number(obj, "imap_port", e->imap_port);
    if (port >= 1 && port <= 65535)
        e->imap_port = (uint16_t)port;
}

static void parse_imap_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    sc_imap_channel_config_t *im = &cfg->channels.imap;
    const char *val;
    val = sc_json_get_string(obj, "imap_host");
    if (val) {
        if (im->imap_host)
            a->free(a->ctx, im->imap_host, strlen(im->imap_host) + 1);
        im->imap_host = sc_strdup(a, val);
    }
    double port = sc_json_get_number(obj, "imap_port", im->imap_port);
    if (port >= 1 && port <= 65535)
        im->imap_port = (uint16_t)port;
    val = sc_json_get_string(obj, "imap_username");
    if (val) {
        if (im->imap_username)
            a->free(a->ctx, im->imap_username, strlen(im->imap_username) + 1);
        im->imap_username = sc_strdup(a, val);
    }
    val = sc_json_get_string(obj, "imap_password");
    if (val) {
        if (im->imap_password)
            a->free(a->ctx, im->imap_password, strlen(im->imap_password) + 1);
        im->imap_password = sc_strdup(a, val);
    }
    val = sc_json_get_string(obj, "imap_folder");
    if (val) {
        if (im->imap_folder)
            a->free(a->ctx, im->imap_folder, strlen(im->imap_folder) + 1);
        im->imap_folder = sc_strdup(a, val);
    }
    im->imap_use_tls = sc_json_get_bool(obj, "imap_use_tls", true);
}

static void parse_imessage_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *t = sc_json_get_string(obj, "default_target");
    if (t) {
        if (cfg->channels.imessage.default_target)
            a->free(a->ctx, cfg->channels.imessage.default_target,
                    strlen(cfg->channels.imessage.default_target) + 1);
        cfg->channels.imessage.default_target = sc_strdup(a, t);
    }
    sc_json_value_t *af = sc_json_object_get(obj, "allow_from");
    if (af && af->type == SC_JSON_ARRAY) {
        if (cfg->channels.imessage.allow_from) {
            for (size_t i = 0; i < cfg->channels.imessage.allow_from_count; i++)
                if (cfg->channels.imessage.allow_from[i])
                    a->free(a->ctx, cfg->channels.imessage.allow_from[i],
                            strlen(cfg->channels.imessage.allow_from[i]) + 1);
            a->free(a->ctx, cfg->channels.imessage.allow_from,
                    cfg->channels.imessage.allow_from_count * sizeof(char *));
        }
        parse_string_array(a, &cfg->channels.imessage.allow_from,
                           &cfg->channels.imessage.allow_from_count, af);
    }
    cfg->channels.imessage.poll_interval_sec =
        (int)sc_json_get_number(obj, "poll_interval_sec", 30.0);
    cfg->channels.imessage.user_response_window_sec =
        (int)sc_json_get_number(obj, "user_response_window_sec", 0.0);

    const char *rm = sc_json_get_string(obj, "response_mode");
    if (rm) {
        if (cfg->channels.imessage.response_mode)
            a->free(a->ctx, cfg->channels.imessage.response_mode,
                    strlen(cfg->channels.imessage.response_mode) + 1);
        cfg->channels.imessage.response_mode = sc_strdup(a, rm);
    }
}

static void parse_gmail_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *cid = sc_json_get_string(obj, "client_id");
    const char *csec = sc_json_get_string(obj, "client_secret");
    const char *rtok = sc_json_get_string(obj, "refresh_token");
    if (cid) {
        if (cfg->channels.gmail.client_id)
            a->free(a->ctx, cfg->channels.gmail.client_id,
                    strlen(cfg->channels.gmail.client_id) + 1);
        cfg->channels.gmail.client_id = sc_strndup(a, cid, strlen(cid));
    }
    if (csec) {
        if (cfg->channels.gmail.client_secret)
            a->free(a->ctx, cfg->channels.gmail.client_secret,
                    strlen(cfg->channels.gmail.client_secret) + 1);
        cfg->channels.gmail.client_secret = sc_strndup(a, csec, strlen(csec));
    }
    if (rtok) {
        if (cfg->channels.gmail.refresh_token)
            a->free(a->ctx, cfg->channels.gmail.refresh_token,
                    strlen(cfg->channels.gmail.refresh_token) + 1);
        cfg->channels.gmail.refresh_token = sc_strndup(a, rtok, strlen(rtok));
    }
    cfg->channels.gmail.poll_interval_sec = (int)sc_json_get_number(obj, "poll_interval_sec", 30.0);
}

static void parse_telegram_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_telegram_channel_config_t *t = &cfg->channels.telegram;

    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;

    const char *s = sc_json_get_string(val, "token");
    if (s) {
        if (t->token)
            a->free(a->ctx, t->token, strlen(t->token) + 1);
        t->token = sc_strdup(a, s);
    }

    sc_json_value_t *af = sc_json_object_get(val, "allow_from");
    if (af && af->type == SC_JSON_ARRAY && af->data.array.items) {
        for (size_t i = 0; i < t->allow_from_count; i++) {
            if (t->allow_from[i])
                a->free(a->ctx, t->allow_from[i], strlen(t->allow_from[i]) + 1);
        }
        t->allow_from_count = 0;
        for (size_t i = 0;
             i < af->data.array.len && t->allow_from_count < SC_TELEGRAM_ALLOW_FROM_MAX; i++) {
            sc_json_value_t *item = af->data.array.items[i];
            if (item && item->type == SC_JSON_STRING && item->data.string.ptr)
                t->allow_from[t->allow_from_count++] = sc_strdup(a, item->data.string.ptr);
        }
    }
}

static void parse_discord_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_discord_channel_config_t *d = &cfg->channels.discord;

    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;

    const char *s = sc_json_get_string(val, "token");
    if (s) {
        if (d->token)
            a->free(a->ctx, d->token, strlen(d->token) + 1);
        d->token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "guild_id");
    if (s) {
        if (d->guild_id)
            a->free(a->ctx, d->guild_id, strlen(d->guild_id) + 1);
        d->guild_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "bot_id");
    if (s) {
        if (d->bot_id)
            a->free(a->ctx, d->bot_id, strlen(d->bot_id) + 1);
        d->bot_id = sc_strdup(a, s);
    }

    sc_json_value_t *ch_ids = sc_json_object_get(val, "channel_ids");
    if (ch_ids && ch_ids->type == SC_JSON_ARRAY && ch_ids->data.array.items) {
        for (size_t i = 0; i < d->channel_ids_count; i++) {
            if (d->channel_ids[i])
                a->free(a->ctx, d->channel_ids[i], strlen(d->channel_ids[i]) + 1);
        }
        d->channel_ids_count = 0;
        for (size_t i = 0;
             i < ch_ids->data.array.len && d->channel_ids_count < SC_DISCORD_CHANNEL_IDS_MAX; i++) {
            sc_json_value_t *item = ch_ids->data.array.items[i];
            if (item && item->type == SC_JSON_STRING && item->data.string.ptr) {
                d->channel_ids[d->channel_ids_count++] = sc_strdup(a, item->data.string.ptr);
            }
        }
    }
}

static void parse_slack_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_slack_channel_config_t *sl = &cfg->channels.slack;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "token");
    if (s) {
        if (sl->token)
            a->free(a->ctx, sl->token, strlen(sl->token) + 1);
        sl->token = sc_strdup(a, s);
    }
    sc_json_value_t *ch_ids = sc_json_object_get(val, "channel_ids");
    if (ch_ids && ch_ids->type == SC_JSON_ARRAY && ch_ids->data.array.items) {
        for (size_t i = 0; i < sl->channel_ids_count; i++)
            if (sl->channel_ids[i])
                a->free(a->ctx, sl->channel_ids[i], strlen(sl->channel_ids[i]) + 1);
        sl->channel_ids_count = 0;
        for (size_t i = 0;
             i < ch_ids->data.array.len && sl->channel_ids_count < SC_SLACK_CHANNEL_IDS_MAX; i++) {
            sc_json_value_t *item = ch_ids->data.array.items[i];
            if (item && item->type == SC_JSON_STRING && item->data.string.ptr)
                sl->channel_ids[sl->channel_ids_count++] = sc_strdup(a, item->data.string.ptr);
        }
    }
}

static void parse_whatsapp_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_whatsapp_channel_config_t *wa = &cfg->channels.whatsapp;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "phone_number_id");
    if (s) {
        if (wa->phone_number_id)
            a->free(a->ctx, wa->phone_number_id, strlen(wa->phone_number_id) + 1);
        wa->phone_number_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "token");
    if (s) {
        if (wa->token)
            a->free(a->ctx, wa->token, strlen(wa->token) + 1);
        wa->token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "verify_token");
    if (s) {
        if (wa->verify_token)
            a->free(a->ctx, wa->verify_token, strlen(wa->verify_token) + 1);
        wa->verify_token = sc_strdup(a, s);
    }
}

static void parse_line_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_line_channel_config_t *ln = &cfg->channels.line;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "channel_token");
    if (s) {
        if (ln->channel_token)
            a->free(a->ctx, ln->channel_token, strlen(ln->channel_token) + 1);
        ln->channel_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "channel_secret");
    if (s) {
        if (ln->channel_secret)
            a->free(a->ctx, ln->channel_secret, strlen(ln->channel_secret) + 1);
        ln->channel_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "user_id");
    if (s) {
        if (ln->user_id)
            a->free(a->ctx, ln->user_id, strlen(ln->user_id) + 1);
        ln->user_id = sc_strdup(a, s);
    }
}

static void parse_google_chat_channel(sc_allocator_t *a, sc_config_t *cfg,
                                      const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_google_chat_channel_config_t *gc = &cfg->channels.google_chat;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "webhook_url");
    if (s) {
        if (gc->webhook_url)
            a->free(a->ctx, gc->webhook_url, strlen(gc->webhook_url) + 1);
        gc->webhook_url = sc_strdup(a, s);
    }
}

static void parse_facebook_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_facebook_channel_config_t *fb = &cfg->channels.facebook;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "page_id");
    if (s) {
        if (fb->page_id)
            a->free(a->ctx, fb->page_id, strlen(fb->page_id) + 1);
        fb->page_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "page_access_token");
    if (s) {
        if (fb->page_access_token)
            a->free(a->ctx, fb->page_access_token, strlen(fb->page_access_token) + 1);
        fb->page_access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "verify_token");
    if (s) {
        if (fb->verify_token)
            a->free(a->ctx, fb->verify_token, strlen(fb->verify_token) + 1);
        fb->verify_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "app_secret");
    if (s) {
        if (fb->app_secret)
            a->free(a->ctx, fb->app_secret, strlen(fb->app_secret) + 1);
        fb->app_secret = sc_strdup(a, s);
    }
}

static void parse_instagram_channel(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_instagram_channel_config_t *ig = &cfg->channels.instagram;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "business_account_id");
    if (s) {
        if (ig->business_account_id)
            a->free(a->ctx, ig->business_account_id, strlen(ig->business_account_id) + 1);
        ig->business_account_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token");
    if (s) {
        if (ig->access_token)
            a->free(a->ctx, ig->access_token, strlen(ig->access_token) + 1);
        ig->access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "verify_token");
    if (s) {
        if (ig->verify_token)
            a->free(a->ctx, ig->verify_token, strlen(ig->verify_token) + 1);
        ig->verify_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "app_secret");
    if (s) {
        if (ig->app_secret)
            a->free(a->ctx, ig->app_secret, strlen(ig->app_secret) + 1);
        ig->app_secret = sc_strdup(a, s);
    }
}

static void parse_twitter_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_twitter_channel_config_t *tw = &cfg->channels.twitter;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "api_key");
    if (s) {
        if (tw->api_key)
            a->free(a->ctx, tw->api_key, strlen(tw->api_key) + 1);
        tw->api_key = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "api_secret");
    if (s) {
        if (tw->api_secret)
            a->free(a->ctx, tw->api_secret, strlen(tw->api_secret) + 1);
        tw->api_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token");
    if (s) {
        if (tw->access_token)
            a->free(a->ctx, tw->access_token, strlen(tw->access_token) + 1);
        tw->access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token_secret");
    if (s) {
        if (tw->access_token_secret)
            a->free(a->ctx, tw->access_token_secret, strlen(tw->access_token_secret) + 1);
        tw->access_token_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "bearer_token");
    if (s) {
        if (tw->bearer_token)
            a->free(a->ctx, tw->bearer_token, strlen(tw->bearer_token) + 1);
        tw->bearer_token = sc_strdup(a, s);
    }
}

static void parse_tiktok_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_tiktok_channel_config_t *tk = &cfg->channels.tiktok;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "client_key");
    if (s) {
        if (tk->client_key)
            a->free(a->ctx, tk->client_key, strlen(tk->client_key) + 1);
        tk->client_key = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "client_secret");
    if (s) {
        if (tk->client_secret)
            a->free(a->ctx, tk->client_secret, strlen(tk->client_secret) + 1);
        tk->client_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token");
    if (s) {
        if (tk->access_token)
            a->free(a->ctx, tk->access_token, strlen(tk->access_token) + 1);
        tk->access_token = sc_strdup(a, s);
    }
}

static void parse_google_rcs_channel(sc_allocator_t *a, sc_config_t *cfg,
                                     const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_google_rcs_channel_config_t *rcs = &cfg->channels.google_rcs;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "agent_id");
    if (s) {
        if (rcs->agent_id)
            a->free(a->ctx, rcs->agent_id, strlen(rcs->agent_id) + 1);
        rcs->agent_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "token");
    if (s) {
        if (rcs->token)
            a->free(a->ctx, rcs->token, strlen(rcs->token) + 1);
        rcs->token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "service_account_json_path");
    if (s) {
        if (rcs->service_account_json_path)
            a->free(a->ctx, rcs->service_account_json_path,
                    strlen(rcs->service_account_json_path) + 1);
        rcs->service_account_json_path = sc_strdup(a, s);
    }
}

static void parse_mqtt_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_mqtt_channel_config_t *mq = &cfg->channels.mqtt;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "broker_url");
    if (s) {
        if (mq->broker_url)
            a->free(a->ctx, mq->broker_url, strlen(mq->broker_url) + 1);
        mq->broker_url = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "inbound_topic");
    if (s) {
        if (mq->inbound_topic)
            a->free(a->ctx, mq->inbound_topic, strlen(mq->inbound_topic) + 1);
        mq->inbound_topic = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "outbound_topic");
    if (s) {
        if (mq->outbound_topic)
            a->free(a->ctx, mq->outbound_topic, strlen(mq->outbound_topic) + 1);
        mq->outbound_topic = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "username");
    if (s) {
        if (mq->username)
            a->free(a->ctx, mq->username, strlen(mq->username) + 1);
        mq->username = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "password");
    if (s) {
        if (mq->password)
            a->free(a->ctx, mq->password, strlen(mq->password) + 1);
        mq->password = sc_strdup(a, s);
    }
    mq->qos = (int)sc_json_get_number(val, "qos", (double)mq->qos);
}

static void parse_matrix_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *hs = sc_json_get_string(obj, "homeserver");
    const char *tok = sc_json_get_string(obj, "access_token");
    if (hs) {
        if (cfg->channels.matrix.homeserver)
            a->free(a->ctx, cfg->channels.matrix.homeserver,
                    strlen(cfg->channels.matrix.homeserver) + 1);
        cfg->channels.matrix.homeserver = sc_strdup(a, hs);
    }
    if (tok) {
        if (cfg->channels.matrix.access_token)
            a->free(a->ctx, cfg->channels.matrix.access_token,
                    strlen(cfg->channels.matrix.access_token) + 1);
        cfg->channels.matrix.access_token = sc_strdup(a, tok);
    }
}

static void parse_irc_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *srv = sc_json_get_string(obj, "server");
    if (srv) {
        if (cfg->channels.irc.server)
            a->free(a->ctx, cfg->channels.irc.server, strlen(cfg->channels.irc.server) + 1);
        cfg->channels.irc.server = sc_strdup(a, srv);
    }
    double port = sc_json_get_number(obj, "port", 6667);
    if (port >= 1 && port <= 65535)
        cfg->channels.irc.port = (uint16_t)port;
}

static void parse_lark_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_lark_channel_config_t *lk = &cfg->channels.lark;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "app_id");
    if (s) {
        if (lk->app_id)
            a->free(a->ctx, lk->app_id, strlen(lk->app_id) + 1);
        lk->app_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "app_secret");
    if (s) {
        if (lk->app_secret)
            a->free(a->ctx, lk->app_secret, strlen(lk->app_secret) + 1);
        lk->app_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "webhook_url");
    if (s) {
        if (lk->webhook_url)
            a->free(a->ctx, lk->webhook_url, strlen(lk->webhook_url) + 1);
        lk->webhook_url = sc_strdup(a, s);
    }
}

static void parse_dingtalk_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_dingtalk_channel_config_t *dt = &cfg->channels.dingtalk;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "app_key");
    if (s) {
        if (dt->app_key)
            a->free(a->ctx, dt->app_key, strlen(dt->app_key) + 1);
        dt->app_key = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "app_secret");
    if (s) {
        if (dt->app_secret)
            a->free(a->ctx, dt->app_secret, strlen(dt->app_secret) + 1);
        dt->app_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "webhook_url");
    if (s) {
        if (dt->webhook_url)
            a->free(a->ctx, dt->webhook_url, strlen(dt->webhook_url) + 1);
        dt->webhook_url = sc_strdup(a, s);
    }
}

static void parse_teams_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_teams_channel_config_t *tm = &cfg->channels.teams;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "webhook_url");
    if (s) {
        if (tm->webhook_url)
            a->free(a->ctx, tm->webhook_url, strlen(tm->webhook_url) + 1);
        tm->webhook_url = sc_strdup(a, s);
    }
}

static void parse_twilio_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_twilio_channel_config_t *tw = &cfg->channels.twilio;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "account_sid");
    if (s) {
        if (tw->account_sid)
            a->free(a->ctx, tw->account_sid, strlen(tw->account_sid) + 1);
        tw->account_sid = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "auth_token");
    if (s) {
        if (tw->auth_token)
            a->free(a->ctx, tw->auth_token, strlen(tw->auth_token) + 1);
        tw->auth_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "from_number");
    if (s) {
        if (tw->from_number)
            a->free(a->ctx, tw->from_number, strlen(tw->from_number) + 1);
        tw->from_number = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "to_number");
    if (s) {
        if (tw->to_number)
            a->free(a->ctx, tw->to_number, strlen(tw->to_number) + 1);
        tw->to_number = sc_strdup(a, s);
    }
}

static void parse_onebot_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_onebot_channel_config_t *ob = &cfg->channels.onebot;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "api_base");
    if (s) {
        if (ob->api_base)
            a->free(a->ctx, ob->api_base, strlen(ob->api_base) + 1);
        ob->api_base = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token");
    if (s) {
        if (ob->access_token)
            a->free(a->ctx, ob->access_token, strlen(ob->access_token) + 1);
        ob->access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "user_id");
    if (s) {
        if (ob->user_id)
            a->free(a->ctx, ob->user_id, strlen(ob->user_id) + 1);
        ob->user_id = sc_strdup(a, s);
    }
}

static void parse_qq_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_qq_channel_config_t *qq = &cfg->channels.qq;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items &&
        obj->data.array.items[0])
        val = obj->data.array.items[0];
    if (!val || val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "app_id");
    if (s) {
        if (qq->app_id)
            a->free(a->ctx, qq->app_id, strlen(qq->app_id) + 1);
        qq->app_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "bot_token");
    if (s) {
        if (qq->bot_token)
            a->free(a->ctx, qq->bot_token, strlen(qq->bot_token) + 1);
        qq->bot_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "channel_id");
    if (s) {
        if (qq->channel_id)
            a->free(a->ctx, qq->channel_id, strlen(qq->channel_id) + 1);
        qq->channel_id = sc_strdup(a, s);
    }
    qq->sandbox = sc_json_get_bool(val, "sandbox", qq->sandbox);
}

static void parse_nostr_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *nak = sc_json_get_string(obj, "nak_path");
    const char *pk = sc_json_get_string(obj, "bot_pubkey");
    const char *relay = sc_json_get_string(obj, "relay_url");
    const char *sk = sc_json_get_string(obj, "seckey_hex");
    if (nak) {
        if (cfg->channels.nostr.nak_path)
            a->free(a->ctx, cfg->channels.nostr.nak_path, strlen(cfg->channels.nostr.nak_path) + 1);
        cfg->channels.nostr.nak_path = sc_strdup(a, nak);
    }
    if (pk) {
        if (cfg->channels.nostr.bot_pubkey)
            a->free(a->ctx, cfg->channels.nostr.bot_pubkey,
                    strlen(cfg->channels.nostr.bot_pubkey) + 1);
        cfg->channels.nostr.bot_pubkey = sc_strdup(a, pk);
    }
    if (relay) {
        if (cfg->channels.nostr.relay_url)
            a->free(a->ctx, cfg->channels.nostr.relay_url,
                    strlen(cfg->channels.nostr.relay_url) + 1);
        cfg->channels.nostr.relay_url = sc_strdup(a, relay);
    }
    if (sk) {
        if (cfg->channels.nostr.seckey_hex)
            a->free(a->ctx, cfg->channels.nostr.seckey_hex,
                    strlen(cfg->channels.nostr.seckey_hex) + 1);
        cfg->channels.nostr.seckey_hex = sc_strdup(a, sk);
    }
}

sc_error_t parse_channels(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->channels.cli = sc_json_get_bool(obj, "cli", cfg->channels.cli);
    cfg->channels.suppress_tool_progress =
        sc_json_get_bool(obj, "suppress_tool_progress", cfg->channels.suppress_tool_progress);
    const char *def_ch = sc_json_get_string(obj, "default_channel");
    if (def_ch) {
        if (cfg->channels.default_channel)
            a->free(a->ctx, cfg->channels.default_channel,
                    strlen(cfg->channels.default_channel) + 1);
        cfg->channels.default_channel = sc_strdup(a, def_ch);
    }

    sc_json_value_t *email_obj = sc_json_object_get(obj, "email");
    if (email_obj)
        parse_email_channel(a, cfg, email_obj);

    sc_json_value_t *imap_obj = sc_json_object_get(obj, "imap");
    if (imap_obj)
        parse_imap_channel(a, cfg, imap_obj);

    sc_json_value_t *imsg_obj = sc_json_object_get(obj, "imessage");
    if (imsg_obj)
        parse_imessage_channel(a, cfg, imsg_obj);

    sc_json_value_t *gmail_obj = sc_json_object_get(obj, "gmail");
    if (gmail_obj)
        parse_gmail_channel(a, cfg, gmail_obj);

    sc_json_value_t *telegram_obj = sc_json_object_get(obj, "telegram");
    if (telegram_obj)
        parse_telegram_channel(a, cfg, telegram_obj);

    sc_json_value_t *discord_obj = sc_json_object_get(obj, "discord");
    if (discord_obj)
        parse_discord_channel(a, cfg, discord_obj);

    sc_json_value_t *slack_obj = sc_json_object_get(obj, "slack");
    if (slack_obj)
        parse_slack_channel(a, cfg, slack_obj);

    sc_json_value_t *whatsapp_obj = sc_json_object_get(obj, "whatsapp");
    if (whatsapp_obj)
        parse_whatsapp_channel(a, cfg, whatsapp_obj);

    sc_json_value_t *line_obj = sc_json_object_get(obj, "line");
    if (line_obj)
        parse_line_channel(a, cfg, line_obj);

    sc_json_value_t *google_chat_obj = sc_json_object_get(obj, "google_chat");
    if (google_chat_obj)
        parse_google_chat_channel(a, cfg, google_chat_obj);

    sc_json_value_t *facebook_obj = sc_json_object_get(obj, "facebook");
    if (facebook_obj)
        parse_facebook_channel(a, cfg, facebook_obj);

    sc_json_value_t *instagram_obj = sc_json_object_get(obj, "instagram");
    if (instagram_obj)
        parse_instagram_channel(a, cfg, instagram_obj);

    sc_json_value_t *twitter_obj = sc_json_object_get(obj, "twitter");
    if (twitter_obj)
        parse_twitter_channel(a, cfg, twitter_obj);

    sc_json_value_t *tiktok_obj = sc_json_object_get(obj, "tiktok");
    if (tiktok_obj)
        parse_tiktok_channel(a, cfg, tiktok_obj);

    sc_json_value_t *google_rcs_obj = sc_json_object_get(obj, "google_rcs");
    if (google_rcs_obj)
        parse_google_rcs_channel(a, cfg, google_rcs_obj);

    sc_json_value_t *mqtt_obj = sc_json_object_get(obj, "mqtt");
    if (mqtt_obj)
        parse_mqtt_channel(a, cfg, mqtt_obj);

    sc_json_value_t *matrix_obj = sc_json_object_get(obj, "matrix");
    if (matrix_obj)
        parse_matrix_channel(a, cfg, matrix_obj);

    sc_json_value_t *irc_obj = sc_json_object_get(obj, "irc");
    if (irc_obj)
        parse_irc_channel(a, cfg, irc_obj);

    sc_json_value_t *nostr_obj = sc_json_object_get(obj, "nostr");
    if (nostr_obj)
        parse_nostr_channel(a, cfg, nostr_obj);

    sc_json_value_t *lark_obj = sc_json_object_get(obj, "lark");
    if (lark_obj)
        parse_lark_channel(a, cfg, lark_obj);

    sc_json_value_t *dingtalk_obj = sc_json_object_get(obj, "dingtalk");
    if (dingtalk_obj)
        parse_dingtalk_channel(a, cfg, dingtalk_obj);

    sc_json_value_t *teams_obj = sc_json_object_get(obj, "teams");
    if (teams_obj)
        parse_teams_channel(a, cfg, teams_obj);

    sc_json_value_t *twilio_obj = sc_json_object_get(obj, "twilio");
    if (twilio_obj)
        parse_twilio_channel(a, cfg, twilio_obj);

    sc_json_value_t *onebot_obj = sc_json_object_get(obj, "onebot");
    if (onebot_obj)
        parse_onebot_channel(a, cfg, onebot_obj);

    sc_json_value_t *qq_obj = sc_json_object_get(obj, "qq");
    if (qq_obj)
        parse_qq_channel(a, cfg, qq_obj);

    cfg->channels.channel_config_len = 0;
    if (obj->data.object.pairs && cfg->channels.channel_config_len < SC_CHANNEL_CONFIG_MAX) {
        for (size_t i = 0; i < obj->data.object.len; i++) {
            sc_json_pair_t *p = &obj->data.object.pairs[i];
            if (!p->key || !p->value)
                continue;
            if (strcmp(p->key, "cli") == 0 || strcmp(p->key, "default_channel") == 0)
                continue;
            size_t cnt = 0;
            if (p->value->type == SC_JSON_ARRAY && p->value->data.array.items)
                cnt = p->value->data.array.len;
            else if (p->value->type == SC_JSON_OBJECT && p->value->data.object.pairs)
                cnt = (p->value->data.object.len > 0) ? 1 : 0;
            else if (p->value->type == SC_JSON_OBJECT || p->value->type == SC_JSON_ARRAY)
                cnt = 1;
            if (cnt == 0)
                continue;
            if (cfg->channels.channel_config_len >= SC_CHANNEL_CONFIG_MAX)
                break;
            size_t klen = p->key_len > 0 ? p->key_len : strlen(p->key);
            char *k = (char *)a->alloc(a->ctx, klen + 1);
            if (!k)
                break;
            memcpy(k, p->key, klen);
            k[klen] = '\0';
            cfg->channels.channel_config_keys[cfg->channels.channel_config_len] = k;
            cfg->channels.channel_config_counts[cfg->channels.channel_config_len] = cnt;
            cfg->channels.channel_config_len++;
        }
    }
    return SC_OK;
}
