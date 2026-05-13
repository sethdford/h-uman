/*
 * imessage_ax.c — Accessibility (AX) framework bridge for iMessage.
 *
 * Step 1 of the iMessage shape refactor — see
 * docs/plans/2026-05-12-imessage-shape-refactor.md.
 *
 * What lives here
 * ===============
 * Every `ax_*` function. These wrap macOS's AXUIElement / Accessibility API
 * to drive Messages.app for two user-facing features:
 *   1. **Typing indicator** — focus the compose field and inject a zero-width
 *      space to make the '...' bubble appear on the recipient's screen.
 *   2. **Tapback reactions** — walk the AX tree to a message bubble and
 *      perform the tapback action (love / like / dislike / laugh / emphasize
 *      / question). Gated behind HU_IMESSAGE_TAPBACK_ENABLED because the
 *      context-menu walk is fragile across macOS versions.
 *
 * Why a separate file
 * ===================
 * Before this refactor, all 430+ LOC of AX code lived inline in imessage.c
 * (4400-LOC god-file). The AX path is platform-specific (Apple-only), has
 * its own header dependencies (ApplicationServices, libproc, AppKit via
 * Obj-C runtime), and its own version-fragility profile (macOS Sequoia
 * blocked the System Events keystroke entitlement; macOS Tahoe restructured
 * Messages.app as SwiftUI). Keeping it in its own file makes those
 * concerns localizable.
 *
 * Visibility
 * ==========
 * Helpers used only within this file remain `static`. The four functions
 * called from other iMessage modules — `ax_open_conversation`,
 * `ax_start_typing`, `ax_stop_typing`, and (under HU_IMESSAGE_TAPBACK_ENABLED)
 * `ax_tapback` — are declared in `src/channels/imessage_internal.h` and have
 * external linkage. They are intentionally NOT in the public header
 * (include/human/channels/imessage.h) — they're not part of the iMessage
 * channel's contract with the daemon.
 *
 * Tier-2 design note (preserved from the original block)
 * ======================================================
 * Three-tier fallback for typing indicators and tapback reactions:
 *   Tier 1: IMCore private framework (dlopen, macOS 14-15, fast, no UI)
 *   Tier 2: Accessibility API (AXUIElement, macOS 14+, bypasses keystroke block)
 *   Tier 3: AppleScript/JXA subprocess (existing, last resort)
 *
 * Tier 2 is the primary innovation: AXUIElementSetAttributeValue and
 * AXUIElementPerformAction use the calling process's Accessibility permission
 * directly — they don't route through System Events and don't need the
 * "send keystrokes" entitlement that macOS Sequoia/Tahoe blocks.
 *
 * IMCore (Tier 1) lives in imessage.c for now; it'll move to its own
 * imessage_imcore.c or merge into imessage_typing.c in a later step.
 */

#include "imessage_internal.h"

#include "human/core/log.h"

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)

#include <ApplicationServices/ApplicationServices.h>
#include <fcntl.h>
#include <libproc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Messages.app PID lookup ────────────────────────────────────────── */
static pid_t ax_messages_pid(void) {
    int count = proc_listallpids(NULL, 0);
    if (count <= 0)
        return 0;
    size_t buf_size = (size_t)(count + 64) * sizeof(pid_t);
    pid_t *pids = (pid_t *)malloc(buf_size);
    if (!pids)
        return 0;
    count = proc_listallpids(pids, (int)buf_size);
    pid_t found = 0;
    for (int i = 0; i < count; i++) {
        char name[64] = {0};
        proc_name(pids[i], name, sizeof(name));
        if (strcmp(name, "Messages") == 0) {
            found = pids[i];
            break;
        }
    }
    free(pids);
    return found;
}

/* ── AX tree walker: find compose text field ────────────────────────── */
#define AX_MAX_DEPTH 25

static AXUIElementRef ax_find_compose_field_recurse(AXUIElementRef elem, int depth) {
    if (depth > AX_MAX_DEPTH)
        return NULL;
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute, (CFTypeRef *)&children) !=
            kAXErrorSuccess ||
        !children)
        return NULL;
    AXUIElementRef result = NULL;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex i = count - 1; i >= 0 && !result; i--) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
        CFStringRef role = NULL;
        if (AXUIElementCopyAttributeValue(child, kAXRoleAttribute, (CFTypeRef *)&role) ==
                kAXErrorSuccess &&
            role) {
            bool is_text = (CFStringCompare(role, CFSTR("AXTextArea"), 0) == kCFCompareEqualTo ||
                            CFStringCompare(role, CFSTR("AXTextField"), 0) == kCFCompareEqualTo);
            CFRelease(role);
            if (is_text) {
                /* macOS 26 Messages (SwiftUI): the compose field has
                 * desc="Message"; message bubbles are AXTextArea desc="text
                 * entry area". Only accept fields with desc="Message" or
                 * AXTextField role (which is never a message bubble). */
                CFStringRef desc = NULL;
                AXUIElementCopyAttributeValue(child, kAXDescriptionAttribute, (CFTypeRef *)&desc);
                bool is_compose = false;
                if (desc) {
                    is_compose = (CFStringCompare(desc, CFSTR("Message"), 0) == kCFCompareEqualTo);
                    CFRelease(desc);
                }
                if (!is_compose) {
                    CFStringRef child_role = NULL;
                    AXUIElementCopyAttributeValue(child, kAXRoleAttribute,
                                                  (CFTypeRef *)&child_role);
                    if (child_role) {
                        is_compose = (CFStringCompare(child_role, CFSTR("AXTextField"), 0) ==
                                      kCFCompareEqualTo);
                        CFRelease(child_role);
                    }
                }
                if (is_compose) {
                    Boolean settable = false;
                    AXUIElementIsAttributeSettable(child, kAXValueAttribute, &settable);
                    if (settable) {
                        CFRetain(child);
                        result = child;
                    }
                }
            }
        } else if (role) {
            CFRelease(role);
        }
        if (!result)
            result = ax_find_compose_field_recurse(child, depth + 1);
    }
    CFRelease(children);
    return result;
}

/* ── Activate Messages.app via NSRunningApplication ──────────────────
 * Stronger than AXFrontmost alone — uses AppKit to force-activate even
 * when the calling process is a background daemon. */
static void ax_activate_messages(pid_t pid) {
    Class ns_running_app = objc_getClass("NSRunningApplication");
    if (!ns_running_app)
        return;
    SEL sel_pid = sel_registerName("runningApplicationWithProcessIdentifier:");
    id app_obj = ((id (*)(id, SEL, pid_t))objc_msgSend)((id)ns_running_app, sel_pid, pid);
    if (!app_obj)
        return;
    /* activateWithOptions: NSApplicationActivateIgnoringOtherApps (1 << 1 = 2) */
    SEL sel_activate = sel_registerName("activateWithOptions:");
    ((BOOL (*)(id, SEL, unsigned long))objc_msgSend)(app_obj, sel_activate, 2UL);
}

/* ── Open Messages conversation reliably ─────────────────────────────
 * Uses imessage:// URL scheme + NSRunningApplication activation + AXFrontmost
 * + AXRaise. Robust against the daemon running in background. */
void ax_open_conversation(const char *recipient, size_t recipient_len) {
    char url[320];
    int n = snprintf(url, sizeof(url), "imessage://%.*s", (int)recipient_len, recipient);
    if (n <= 0 || (size_t)n >= sizeof(url))
        return;
    pid_t child = fork();
    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("open", "open", url, NULL);
        _exit(127);
    } else if (child > 0) {
        int status = 0;
        waitpid(child, &status, 0);
    }
    usleep(300000); /* 300ms for URL handling */

    pid_t pid = ax_messages_pid();
    if (pid <= 0)
        return;

    /* NSRunningApplication activation — strongest method for background daemons */
    ax_activate_messages(pid);

    /* Also set AXFrontmost as a belt-and-suspenders approach */
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (app) {
        AXError aerr = AXUIElementSetAttributeValue(app, CFSTR("AXFrontmost"), kCFBooleanTrue);
        if (aerr != kAXErrorSuccess)
            hu_log_info("imessage", NULL, "AX setFrontmost: error %d", (int)aerr);

        /* AXRaise on the first window for extra robustness */
        CFArrayRef windows = NULL;
        AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
        if (windows && CFArrayGetCount(windows) > 0) {
            AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
            AXUIElementPerformAction(win, CFSTR("AXRaise"));
        }
        if (windows)
            CFRelease(windows);
        CFRelease(app);
    }
    usleep(500000); /* 500ms for window to appear after activation */
}

/* ── AX window helper ───────────────────────────────────────────────
 * macOS 26 Messages: windows may not exist when running in the background.
 * Try focused window first, then any AXWindow, then the first top-level
 * child element (SwiftUI apps sometimes expose the main view as a non-window). */
static AXUIElementRef ax_get_messages_window(void) {
    pid_t pid = ax_messages_pid();
    if (pid == 0)
        return NULL;
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app)
        return NULL;
    AXUIElementRef window = NULL;
    AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, (CFTypeRef *)&window);
    if (!window) {
        CFArrayRef windows = NULL;
        AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
        if (windows && CFArrayGetCount(windows) > 0) {
            window = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
            CFRetain(window);
        }
        if (windows)
            CFRelease(windows);
    }
    if (!window) {
        /* macOS 26 fallback: check top-level children for any element with children
         * (the SwiftUI main view appears as a role-less child of the application). */
        CFArrayRef children = NULL;
        AXUIElementCopyAttributeValue(app, kAXChildrenAttribute, (CFTypeRef *)&children);
        if (children) {
            CFIndex count = CFArrayGetCount(children);
            for (CFIndex i = 0; i < count; i++) {
                AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                CFStringRef role = NULL;
                AXUIElementCopyAttributeValue(child, kAXRoleAttribute, (CFTypeRef *)&role);
                bool is_menu =
                    (role && CFStringCompare(role, CFSTR("AXMenuBar"), 0) == kCFCompareEqualTo);
                if (role)
                    CFRelease(role);
                if (!is_menu) {
                    CFArrayRef sub = NULL;
                    AXUIElementCopyAttributeValue(child, kAXChildrenAttribute, (CFTypeRef *)&sub);
                    if (sub) {
                        CFIndex sub_count = CFArrayGetCount(sub);
                        CFRelease(sub);
                        if (sub_count > 0) {
                            CFRetain(child);
                            window = child;
                            break;
                        }
                    }
                }
            }
            CFRelease(children);
        }
    }
    CFRelease(app);
    return window;
}

/* ── AX: start typing via compose field value injection ─────────────── */
bool ax_start_typing(const char *target, size_t target_len) {
    ax_open_conversation(target, target_len);

    /* Retry loop: Messages.app may need time to fully activate and populate
     * its AX tree, especially when the daemon runs in the background.
     * Try up to 8 times with 200ms intervals (~1.6s max additional wait). */
    AXUIElementRef field = NULL;
    for (int attempt = 0; attempt < 8 && !field; attempt++) {
        if (attempt > 0) {
            usleep(200000); /* 200ms between retries */
            /* Re-activate on retries 2 and 5 in case focus was stolen */
            if (attempt == 2 || attempt == 5) {
                pid_t pid = ax_messages_pid();
                if (pid > 0)
                    ax_activate_messages(pid);
            }
        }
        AXUIElementRef window = ax_get_messages_window();
        if (!window)
            continue;
        field = ax_find_compose_field_recurse(window, 0);
        CFRelease(window);
    }
    if (!field) {
        hu_log_info("imessage", NULL, "AX typing: compose field not found after retries");
        return false;
    }

    AXUIElementSetAttributeValue(field, kAXFocusedAttribute, kCFBooleanTrue);

    /* Inject a zero-width space — Messages sends the typing indicator when
     * the compose field has content. Invisible if the user glances at screen. */
    CFStringRef marker = CFSTR("\xE2\x80\x8B"); /* U+200B ZERO WIDTH SPACE */
    AXError set_err = AXUIElementSetAttributeValue(field, kAXValueAttribute, marker);
    CFRelease(field);
    if (set_err != kAXErrorSuccess)
        hu_log_info("imessage", NULL, "AX typing: set value failed (%d)", (int)set_err);
    return (set_err == kAXErrorSuccess);
}

/* ── AX: stop typing by clearing compose field ──────────────────────── */
bool ax_stop_typing(void) {
    AXUIElementRef window = ax_get_messages_window();
    if (!window)
        return false;
    AXUIElementRef field = ax_find_compose_field_recurse(window, 0);
    CFRelease(window);
    if (!field)
        return false;
    CFStringRef empty = CFSTR("");
    AXError err = AXUIElementSetAttributeValue(field, kAXValueAttribute, empty);
    CFRelease(field);
    return (err == kAXErrorSuccess);
}

#ifdef HU_IMESSAGE_TAPBACK_ENABLED
/* ── AX tree: find message element for tapback ──────────────────────
 * macOS 26 (Tahoe): Messages uses SwiftUI; transcript is nested AXGroups.
 * Each message bubble is an AXGroup whose description contains the message
 * text (e.g. "Your iMessage, hello world, 3:54 PM"). Children contain
 * AXTextArea elements with desc="text entry area" holding the actual text.
 * We match by walking AXGroup descriptions or child AXTextArea values. */
static AXUIElementRef ax_find_message_group(AXUIElementRef elem, const char *content_prefix,
                                            int depth) {
    if (depth > AX_MAX_DEPTH || !content_prefix || !content_prefix[0])
        return NULL;
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute, (CFTypeRef *)&children) !=
            kAXErrorSuccess ||
        !children)
        return NULL;

    AXUIElementRef found = NULL;
    CFIndex count = CFArrayGetCount(children);
    /* Walk from bottom (most recent messages last). */
    for (CFIndex i = count - 1; i >= 0 && !found; i--) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);

        /* Check this element's description for message text. */
        CFStringRef desc = NULL;
        if (AXUIElementCopyAttributeValue(child, kAXDescriptionAttribute, (CFTypeRef *)&desc) ==
                kAXErrorSuccess &&
            desc) {
            char dbuf[512] = {0};
            CFStringGetCString(desc, dbuf, (CFIndex)sizeof(dbuf), kCFStringEncodingUTF8);
            CFRelease(desc);
            if (strstr(dbuf, content_prefix)) {
                /* Check this element supports AXShowMenu (for context menu). */
                CFArrayRef actions = NULL;
                if (AXUIElementCopyActionNames(child, &actions) == kAXErrorSuccess && actions) {
                    CFIndex ac = CFArrayGetCount(actions);
                    for (CFIndex a = 0; a < ac; a++) {
                        CFStringRef act = (CFStringRef)CFArrayGetValueAtIndex(actions, a);
                        if (CFStringCompare(act, CFSTR("AXShowMenu"), 0) == kCFCompareEqualTo) {
                            CFRetain(child);
                            found = child;
                            break;
                        }
                    }
                    CFRelease(actions);
                }
            }
        }

        /* Also check child AXTextArea values. */
        if (!found) {
            CFStringRef role = NULL;
            AXUIElementCopyAttributeValue(child, kAXRoleAttribute, (CFTypeRef *)&role);
            bool is_textarea =
                (role && CFStringCompare(role, CFSTR("AXTextArea"), 0) == kCFCompareEqualTo);
            if (role)
                CFRelease(role);
            if (is_textarea) {
                CFStringRef val = NULL;
                if (AXUIElementCopyAttributeValue(child, kAXValueAttribute, (CFTypeRef *)&val) ==
                        kAXErrorSuccess &&
                    val) {
                    char vbuf[512] = {0};
                    CFStringGetCString(val, vbuf, (CFIndex)sizeof(vbuf), kCFStringEncodingUTF8);
                    CFRelease(val);
                    if (strstr(vbuf, content_prefix)) {
                        /* Return the PARENT (which has AXShowMenu), not the text area. */
                        CFRetain(elem);
                        found = elem;
                    }
                }
            }
        }

        if (!found)
            found = ax_find_message_group(child, content_prefix, depth + 1);
    }
    CFRelease(children);
    return found;
}

/* ── AX: perform tapback reaction ───────────────────────────────────
 * macOS 26 (Tahoe): SwiftUI Messages exposes tapbacks as custom AX actions
 * on the message element (e.g. "Name:Heart\nTarget:0x0\nSelector:(null)").
 * We enumerate actions on the message and its inner child, then perform
 * the one matching our desired tapback prefix. Pure AX — no CGEvent. */
static bool ax_perform_tapback_on_row(AXUIElementRef row, const char *tapback_label) {
    /* Check both the row and its first child (SwiftUI nests the actions
     * on the inner group, not the outer description group). */
    AXUIElementRef targets[2] = {row, NULL};
    int target_count = 1;
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(row, kAXChildrenAttribute, (CFTypeRef *)&children) ==
            kAXErrorSuccess &&
        children) {
        if (CFArrayGetCount(children) > 0) {
            targets[1] = (AXUIElementRef)CFArrayGetValueAtIndex(children, 0);
            target_count = 2;
        }
    }

    bool success = false;
    size_t label_len = strlen(tapback_label);
    for (int t = 0; t < target_count && !success; t++) {
        CFArrayRef actions = NULL;
        if (AXUIElementCopyActionNames(targets[t], &actions) != kAXErrorSuccess || !actions)
            continue;
        CFIndex ac = CFArrayGetCount(actions);
        for (CFIndex a = 0; a < ac && !success; a++) {
            CFStringRef act_name = (CFStringRef)CFArrayGetValueAtIndex(actions, a);
            char abuf[256] = {0};
            CFStringGetCString(act_name, abuf, (CFIndex)sizeof(abuf), kCFStringEncodingUTF8);
            if (strncmp(abuf, tapback_label, label_len) == 0) {
                AXError err = AXUIElementPerformAction(targets[t], act_name);
                success = (err == kAXErrorSuccess);
            }
        }
        CFRelease(actions);
    }

    if (children)
        CFRelease(children);
    return success;
}

bool ax_tapback(const char *content_prefix, int row_offset, const char *tapback_label) {
    (void)row_offset;

    /* Retry loop: window or message may not be in AX tree immediately */
    AXUIElementRef msg_group = NULL;
    for (int attempt = 0; attempt < 6 && !msg_group; attempt++) {
        if (attempt > 0) {
            usleep(250000); /* 250ms between retries */
            if (attempt == 3) {
                pid_t pid = ax_messages_pid();
                if (pid > 0)
                    ax_activate_messages(pid);
            }
        }
        AXUIElementRef window = ax_get_messages_window();
        if (!window)
            continue;
        msg_group = ax_find_message_group(window, content_prefix, 0);
        CFRelease(window);
    }
    if (!msg_group) {
        hu_log_info("imessage", NULL, "AX tapback: message not found after retries");
        return false;
    }

    bool ok = ax_perform_tapback_on_row(msg_group, tapback_label);
    CFRelease(msg_group);
    hu_log_info("imessage", NULL, "AX tapback: %s (action=%s)", ok ? "sent" : "action not found",
                tapback_label);
    return ok;
}
#endif /* HU_IMESSAGE_TAPBACK_ENABLED */

/* ── AX trust check (public API, used by doctor) ──────────────────────
 *
 * W10 — distinct from Full Disk Access. AX permission is granted in
 * System Settings → Privacy & Security → Accessibility, while FDA is
 * granted under "Full Disk Access" in the same pane. The daemon needs
 * both for full functionality but doctor should tell the user which
 * one is missing. */
bool hu_imessage_ax_is_trusted(void) {
    return (bool)AXIsProcessTrusted();
}

#endif /* !HU_IS_TEST && __APPLE__ && __MACH__ */
