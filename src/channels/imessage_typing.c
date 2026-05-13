/*
 * imessage_typing.c — Typing indicator: three-tier fallback.
 *
 * Step 2 of the iMessage shape refactor — see
 * docs/plans/2026-05-12-imessage-shape-refactor.md.
 *
 * Tiered fallback for showing the '…' typing bubble on the recipient's
 * device:
 *
 *   Tier 1: IMCore (private framework, dlopen, macOS 14–15, fast, no UI).
 *           Direct API: IMChatRegistry → existingChatWithChatIdentifier:
 *           → setLocalUserIsTyping:. Best UX (no Messages.app activation),
 *           but locked down on macOS 26+ by entitlement.
 *
 *   Tier 2: Accessibility API — implemented in src/channels/imessage_ax.c.
 *           Focuses the compose field, injects a zero-width space.
 *
 *   Tier 3: AppleScript / System Events keystroke (this file's
 *           `imessage_simulate_typing`). Last resort — works across more
 *           macOS versions but requires Accessibility permission and
 *           briefly steals focus.
 *
 * The vtable hooks `imessage_start_typing` and `imessage_stop_typing` walk
 * the tiers in order. Both are exported via imessage_internal.h so the
 * channel vtable in imessage.c can point to them.
 *
 * `imessage_simulate_typing` is also exported because the send path calls
 * it directly when the daemon hasn't already triggered a typing indicator
 * (e.g. quick send with no pre-trigger).
 */

#include "imessage_internal.h"

#include "human/channel.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/process_util.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <unistd.h>
#endif

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)

/* ── IMCore (Tier-1) private framework bridge ───────────────────────── */

static bool imcore_init(hu_imessage_ctx_t *c) {
    if (!c || c->imcore_tried)
        return c ? c->imcore_connected : false;
    c->imcore_tried = true;

    c->imcore_handle =
        dlopen("/System/Library/PrivateFrameworks/IMCore.framework/IMCore", RTLD_LAZY);
    if (!c->imcore_handle)
        return false;

    Class daemon_cls = (Class)objc_getClass("IMDaemonController");
    if (!daemon_cls) {
        dlclose(c->imcore_handle);
        c->imcore_handle = NULL;
        return false;
    }

    typedef id (*id_msg)(id, SEL);
    id controller = ((id_msg)objc_msgSend)((id)daemon_cls, sel_registerName("sharedInstance"));
    if (!controller) {
        dlclose(c->imcore_handle);
        c->imcore_handle = NULL;
        return false;
    }

    /* Try connecting to the imagent daemon. Fails on macOS 26+ due to
     * private entitlement lockdown (com.apple.imagent.desktop.auth). */
    typedef void (*void_msg)(id, SEL);
    ((void_msg)objc_msgSend)(controller, sel_registerName("connectToDaemon"));

    typedef BOOL (*bool_msg)(id, SEL);
    BOOL connected = ((bool_msg)objc_msgSend)(controller, sel_registerName("isConnected"));
    c->imcore_connected = (connected != 0);
    if (!c->imcore_connected) {
        hu_log_info("imessage", NULL,
                    "IMCore loaded but daemon connection failed "
                    "(expected on macOS 26+, falling back to AX)");
    }
    return c->imcore_connected;
}

static bool imcore_start_typing(hu_imessage_ctx_t *c, const char *recipient, size_t recipient_len) {
    if (!c || !c->imcore_connected || !recipient || recipient_len == 0)
        return false;

    Class registry_cls = (Class)objc_getClass("IMChatRegistry");
    if (!registry_cls)
        return false;

    typedef id (*id_msg)(id, SEL, ...);
    id registry = ((id_msg)objc_msgSend)((id)registry_cls, sel_registerName("sharedInstance"));
    if (!registry)
        return false;

    Class ns_string = (Class)objc_getClass("NSString");
    if (!ns_string)
        return false;

    /* macOS 26 uses "any;-;" prefix; older versions use "iMessage;-;" / "SMS;-;". */
    static const char *prefixes[] = {"iMessage;-;", "SMS;-;", "any;-;"};
    char chat_id[320];
    id chat = NULL;
    for (int px = 0; px < 3 && !chat; px++) {
        int n = snprintf(chat_id, sizeof(chat_id), "%s%.*s", prefixes[px], (int)recipient_len,
                         recipient);
        if (n < 0 || (size_t)n >= sizeof(chat_id))
            continue;
        id chat_id_str = ((id_msg)objc_msgSend)((id)ns_string,
                                                sel_registerName("stringWithUTF8String:"), chat_id);
        if (chat_id_str)
            chat = ((id_msg)objc_msgSend)(
                registry, sel_registerName("existingChatWithChatIdentifier:"), chat_id_str);
    }
    if (!chat)
        return false;

    typedef void (*bool_set_msg)(id, SEL, BOOL);
    ((bool_set_msg)objc_msgSend)(chat, sel_registerName("setLocalUserIsTyping:"), (BOOL)1);
    return true;
}

static bool imcore_stop_typing(hu_imessage_ctx_t *c, const char *recipient, size_t recipient_len) {
    if (!c || !c->imcore_connected || !recipient || recipient_len == 0)
        return false;

    Class registry_cls = (Class)objc_getClass("IMChatRegistry");
    if (!registry_cls)
        return false;

    typedef id (*id_msg)(id, SEL, ...);
    id registry = ((id_msg)objc_msgSend)((id)registry_cls, sel_registerName("sharedInstance"));
    if (!registry)
        return false;

    Class ns_string = (Class)objc_getClass("NSString");
    if (!ns_string)
        return false;

    static const char *prefixes[] = {"iMessage;-;", "SMS;-;", "any;-;"};
    char chat_id[320];
    id chat = NULL;
    for (int px = 0; px < 3 && !chat; px++) {
        int n = snprintf(chat_id, sizeof(chat_id), "%s%.*s", prefixes[px], (int)recipient_len,
                         recipient);
        if (n < 0 || (size_t)n >= sizeof(chat_id))
            continue;
        id chat_id_str = ((id_msg)objc_msgSend)((id)ns_string,
                                                sel_registerName("stringWithUTF8String:"), chat_id);
        if (chat_id_str)
            chat = ((id_msg)objc_msgSend)(
                registry, sel_registerName("existingChatWithChatIdentifier:"), chat_id_str);
    }
    if (!chat)
        return false;

    typedef void (*bool_set_msg)(id, SEL, BOOL);
    ((bool_set_msg)objc_msgSend)(chat, sel_registerName("setLocalUserIsTyping:"), (BOOL)0);
    return true;
}

/* ── Tier-3 AppleScript typing simulator ────────────────────────────── */

/* Typing indicator with chat ID caching and group chat support. Caches
 * target to skip expensive chat iteration on repeat sends. Skipped when
 * the daemon already called start_typing (typing_active). */
void imessage_simulate_typing(hu_imessage_ctx_t *c, const char *tgt, size_t tgt_len,
                              size_t message_len) {
    if (!c || atomic_load(&c->typing_active))
        return;

    unsigned int delay_ms =
        hu_imessage_typing_duration(message_len, (uint32_t)time(NULL) ^ (uint32_t)message_len);

    size_t tgt_esc_cap = tgt_len * 2 + 1;
    if (tgt_esc_cap > 4096)
        return;

    char tgt_esc[4096];
    escape_for_applescript(tgt_esc, sizeof(tgt_esc), tgt, tgt_len);

    if (delay_ms <= 3000) {
        bool same_target = (c->typing_last_target_len == tgt_len && tgt_len > 0 &&
                            memcmp(c->typing_last_target, tgt, tgt_len) == 0);
        if (tgt_len > 0 && tgt_len < sizeof(c->typing_last_target)) {
            memcpy(c->typing_last_target, tgt, tgt_len);
            c->typing_last_target[tgt_len] = '\0';
            c->typing_last_target_len = tgt_len;
        }

        char typing_script[1024];
        int ts_n;
        if (same_target) {
            ts_n = snprintf(typing_script, sizeof(typing_script),
                            "tell application \"Messages\" to activate\n"
                            "delay 0.2\n"
                            "tell application \"System Events\" to tell process \"Messages\"\n"
                            "  keystroke \".\"\n"
                            "  delay %.1f\n"
                            "  keystroke \"a\" using command down\n"
                            "  key code 51\n"
                            "end tell",
                            (float)delay_ms / 1000.0f);
        } else {
            ts_n = snprintf(typing_script, sizeof(typing_script),
                            "tell application \"Messages\"\n"
                            "  activate\n"
                            "  set targetHandle to \"%s\"\n"
                            "  set targetChat to missing value\n"
                            "  repeat with c in every chat\n"
                            "    try\n"
                            "      repeat with p in participants of c\n"
                            "        if handle of p is targetHandle then\n"
                            "          set targetChat to c\n"
                            "          exit repeat\n"
                            "        end if\n"
                            "      end repeat\n"
                            "    end try\n"
                            "    if targetChat is not missing value then exit repeat\n"
                            "  end repeat\n"
                            "end tell\n"
                            "delay 0.3\n"
                            "tell application \"System Events\" to tell process \"Messages\"\n"
                            "  keystroke \".\"\n"
                            "  delay %.1f\n"
                            "  keystroke \"a\" using command down\n"
                            "  key code 51\n"
                            "end tell",
                            tgt_esc, (float)delay_ms / 1000.0f);
        }
        if (ts_n > 0 && (size_t)ts_n < sizeof(typing_script)) {
            const char *ts_argv[] = {"osascript", "-e", typing_script, NULL};
            hu_run_result_t ts_result = {0};
            hu_error_t ts_err = hu_process_run(c->alloc, ts_argv, NULL, 65536, &ts_result);
            hu_run_result_free(c->alloc, &ts_result);
            if (ts_err != HU_OK && getenv("HU_DEBUG"))
                hu_log_error("imessage", NULL, "typing indicator failed (accessibility?)");
        }
    } else {
        /* Longer messages: re-trigger typing indicator every ~2.5s so the
         * bubble stays visible for the entire simulated composing period. */
        bool same_target = (c->typing_last_target_len == tgt_len && tgt_len > 0 &&
                            memcmp(c->typing_last_target, tgt, tgt_len) == 0);
        if (tgt_len > 0 && tgt_len < sizeof(c->typing_last_target)) {
            memcpy(c->typing_last_target, tgt, tgt_len);
            c->typing_last_target[tgt_len] = '\0';
            c->typing_last_target_len = tgt_len;
        }

        unsigned int remaining = delay_ms;
        while (remaining > 0) {
            unsigned int chunk = remaining > 2500 ? 2500 : remaining;
            char typing_script[1024];
            int ts_n;
            if (same_target) {
                ts_n = snprintf(typing_script, sizeof(typing_script),
                                "tell application \"Messages\" to activate\n"
                                "delay 0.2\n"
                                "tell application \"System Events\" to tell process "
                                "\"Messages\"\n"
                                "  keystroke \".\"\n"
                                "  delay %.1f\n"
                                "  keystroke \"a\" using command down\n"
                                "  key code 51\n"
                                "end tell",
                                (float)chunk / 1000.0f);
            } else {
                ts_n = snprintf(typing_script, sizeof(typing_script),
                                "tell application \"Messages\"\n"
                                "  activate\n"
                                "  set targetHandle to \"%s\"\n"
                                "  set targetChat to missing value\n"
                                "  repeat with c in every chat\n"
                                "    try\n"
                                "      repeat with p in participants of c\n"
                                "        if handle of p is targetHandle then\n"
                                "          set targetChat to c\n"
                                "          exit repeat\n"
                                "        end if\n"
                                "      end repeat\n"
                                "    end try\n"
                                "    if targetChat is not missing value then exit repeat\n"
                                "  end repeat\n"
                                "end tell\n"
                                "delay 0.3\n"
                                "tell application \"System Events\" to tell process "
                                "\"Messages\"\n"
                                "  keystroke \".\"\n"
                                "  delay %.1f\n"
                                "  keystroke \"a\" using command down\n"
                                "  key code 51\n"
                                "end tell",
                                tgt_esc, (float)chunk / 1000.0f);
                same_target = true;
            }
            if (ts_n > 0 && (size_t)ts_n < sizeof(typing_script)) {
                const char *ts_argv[] = {"osascript", "-e", typing_script, NULL};
                hu_run_result_t ts_result = {0};
                hu_error_t ts_err = hu_process_run(c->alloc, ts_argv, NULL, 65536, &ts_result);
                hu_run_result_free(c->alloc, &ts_result);
                if (ts_err != HU_OK) {
                    usleep((unsigned int)(remaining) * 1000);
                    break;
                }
            } else {
                usleep((unsigned int)(remaining) * 1000);
                break;
            }
            remaining -= chunk;
        }
    }
}

#endif /* !HU_IS_TEST && __APPLE__ && __MACH__ */

/* ── Vtable hooks: start_typing / stop_typing ────────────────────────── */

hu_error_t imessage_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
#if HU_IS_TEST
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_OK;
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc || !recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (recipient_len > 0 && recipient_len < sizeof(c->typing_last_target)) {
        memcpy(c->typing_last_target, recipient, recipient_len);
        c->typing_last_target[recipient_len] = '\0';
        c->typing_last_target_len = recipient_len;
    }

    /* Tier 1: IMCore — direct API, no UI activation needed. */
    imcore_init(c);
    if (imcore_start_typing(c, recipient, recipient_len)) {
        hu_log_info("imessage", NULL, "typing started via IMCore");
        atomic_store(&c->typing_active, true);
        return HU_OK;
    }

    /* Tier 2: AX compose field injection — no keystrokes, uses our process's
     * Accessibility permission directly. ax_start_typing opens the conversation
     * via imessage:// URL scheme before manipulating the compose field. */
    if (ax_start_typing(recipient, recipient_len)) {
        hu_log_info("imessage", NULL, "typing started via AX compose field");
        atomic_store(&c->typing_active, true);
        return HU_OK;
    }

    /* Tier 3: imsg typing CLI or AppleScript keystroke (legacy). */
    if (c->use_imsg_cli && imsg_cli_available(c)) {
        char tgt_buf[256];
        size_t tb = recipient_len < sizeof(tgt_buf) - 1 ? recipient_len : sizeof(tgt_buf) - 1;
        memcpy(tgt_buf, recipient, tb);
        tgt_buf[tb] = '\0';
        const char *argv[] = {"imsg", "typing", "--to", tgt_buf, "--duration", "5s", NULL};
        hu_run_result_t result = {0};
        hu_error_t err = hu_process_run(c->alloc, argv, NULL, 4096, &result);
        bool ok = (err == HU_OK && result.exit_code == 0);
        hu_run_result_free(c->alloc, &result);
        if (ok) {
            hu_log_info("imessage", NULL, "typing started via imsg CLI");
            atomic_store(&c->typing_active, true);
            return HU_OK;
        }
    }

    hu_log_info("imessage", NULL, "all typing tiers failed (IMCore/AX/imsg)");
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t imessage_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
#if HU_IS_TEST
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_OK;
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    /* Tier 1: IMCore */
    if (imcore_stop_typing(c, recipient, recipient_len)) {
        atomic_store(&c->typing_active, false);
        return HU_OK;
    }

    /* Tier 2: AX — clear compose field */
    if (ax_stop_typing()) {
        atomic_store(&c->typing_active, false);
        return HU_OK;
    }

    /* Tier 3: imsg CLI or AppleScript (legacy) */
    if (c->use_imsg_cli && imsg_cli_available(c) && recipient && recipient_len > 0) {
        char tgt_buf[256];
        size_t tb = recipient_len < sizeof(tgt_buf) - 1 ? recipient_len : sizeof(tgt_buf) - 1;
        memcpy(tgt_buf, recipient, tb);
        tgt_buf[tb] = '\0';
        const char *argv[] = {"imsg", "typing", "--to", tgt_buf, "--stop", "true", NULL};
        hu_run_result_t result = {0};
        hu_error_t err = hu_process_run(c->alloc, argv, NULL, 4096, &result);
        bool ok = (err == HU_OK && result.exit_code == 0);
        hu_run_result_free(c->alloc, &result);
        if (ok) {
            atomic_store(&c->typing_active, false);
            return HU_OK;
        }
    }

    atomic_store(&c->typing_active, false);
    return HU_OK;
#endif
}
