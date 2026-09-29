/* Native Messages voice delivery — macOS port (spec 2026-09-26 W3).
 *
 * Implements hu_voice_record_port_t with CoreAudio (default-input switch and
 * read-back, mic-in-use), AudioQueue (play the clip into BlackHole), AX (press
 * Messages' buttons by label, remember/restore the frontmost app) and IOKit
 * (user idle time). The decisions live in imessage_voice_record.c; this file
 * only performs effects. Test and non-Apple builds get a stub that always
 * blocks at preflight. */
#include "human/channels/imessage_voice_record.h"

#include <stddef.h>
#include <string.h>

#if defined(__APPLE__) && defined(__MACH__) && !HU_IS_TEST && defined(HU_HAS_IMESSAGE) && \
    HU_HAS_IMESSAGE

#include "human/core/allocator.h"
#include "human/core/log.h"
#include "human/core/paths.h"
#include "human/core/process_util.h"

#include <ApplicationServices/ApplicationServices.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <IOKit/IOKitLib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define VREC_AX_MAX_DEPTH 40
#define VREC_BUF_BYTES    0x10000u
#define VREC_NUM_BUFS     3

typedef struct {
    pid_t front_pid;
    char win_title[256];
    AudioFileID file;
    AudioQueueRef queue;
    AudioQueueBufferRef bufs[VREC_NUM_BUFS];
    AudioStreamPacketDescription *pkt_descs;
    UInt32 pkts_per_buf;
    UInt32 buf_bytes;
    SInt64 next_packet;
    double duration_sec;
    atomic_int done_reading;
    atomic_int started;
} vrec_mac_ctx_t;

static vrec_mac_ctx_t g_vrec;

/* ── CoreAudio ─────────────────────────────────────────────────────────── */

static bool dev_name(AudioObjectID id, char *buf, size_t cap) {
    AudioObjectPropertyAddress a = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef s = NULL;
    UInt32 sz = (UInt32)sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, (void *)&s) != noErr || !s)
        return false;
    bool ok = CFStringGetCString(s, buf, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(s);
    return ok;
}

static bool dev_has_input(AudioObjectID id) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                                    kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    return AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) == noErr && sz > 0;
}

/* An input-capable device with this exact name, or kAudioObjectUnknown. */
static AudioObjectID find_input_device(const char *name) {
    if (!name || !name[0])
        return kAudioObjectUnknown;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz) != noErr ||
        sz == 0)
        return kAudioObjectUnknown;
    AudioObjectID *ids = (AudioObjectID *)malloc(sz);
    if (!ids)
        return kAudioObjectUnknown;
    AudioObjectID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) == noErr) {
        UInt32 n = sz / (UInt32)sizeof(AudioObjectID);
        for (UInt32 i = 0; i < n && found == kAudioObjectUnknown; i++) {
            char nm[256];
            if (dev_name(ids[i], nm, sizeof(nm)) && strcmp(nm, name) == 0 && dev_has_input(ids[i]))
                found = ids[i];
        }
    }
    free(ids);
    return found;
}

static bool dev_running_somewhere(AudioObjectID id) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceIsRunningSomewhere,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 v = 0, sz = sizeof(v);
    return AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &v) == noErr && v != 0;
}

static AudioObjectPropertyAddress default_input_addr(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultInputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    return a;
}

static hu_error_t mac_get_input(void *ctx, char *buf, size_t cap) {
    (void)ctx;
    if (!buf || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    buf[0] = '\0';
    AudioObjectPropertyAddress a = default_input_addr();
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 sz = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &id) != noErr ||
        id == kAudioObjectUnknown)
        return HU_ERR_IO;
    return dev_name(id, buf, cap) ? HU_OK : HU_ERR_IO;
}

static hu_error_t mac_set_input(void *ctx, const char *device_name) {
    AudioObjectID id = find_input_device(device_name);
    if (id == kAudioObjectUnknown)
        return HU_ERR_NOT_FOUND;
    AudioObjectPropertyAddress a = default_input_addr();
    if (AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof(id), &id) != noErr)
        return HU_ERR_IO;
    /* The switch is not always visible immediately; settle up to 500 ms. */
    for (int i = 0; i < 10; i++) {
        char cur[256];
        if (mac_get_input(ctx, cur, sizeof(cur)) == HU_OK && strcmp(cur, device_name) == 0)
            return HU_OK;
        usleep(50000);
    }
    return HU_OK; /* the orchestrator's read-back decides */
}

/* ── IOKit idle time ───────────────────────────────────────────────────── */

static double hid_idle_sec(void) {
    io_iterator_t it = 0;
    double secs = 0.0; /* unknown reads as "active": fail closed */
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"), &it) !=
        KERN_SUCCESS)
        return secs;
    io_registry_entry_t e = IOIteratorNext(it);
    if (e) {
        CFTypeRef v =
            IORegistryEntryCreateCFProperty(e, CFSTR("HIDIdleTime"), kCFAllocatorDefault, 0);
        if (v) {
            int64_t ns = 0;
            if (CFGetTypeID(v) == CFNumberGetTypeID() &&
                CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &ns))
                secs = (double)ns / 1e9;
            CFRelease(v);
        }
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return secs;
}

/* ── Messages + AX ─────────────────────────────────────────────────────── */

static AXUIElementRef messages_window(void) {
    pid_t pid = hu_imessage_messages_pid();
    if (pid <= 0)
        return NULL;
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app)
        return NULL;
    CFTypeRef w = NULL;
    if (AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &w) != kAXErrorSuccess ||
        !w) {
        CFArrayRef wins = NULL;
        if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&wins) ==
                kAXErrorSuccess &&
            wins) {
            if (CFArrayGetCount(wins) > 0) {
                w = CFArrayGetValueAtIndex(wins, 0);
                CFRetain(w);
            }
            CFRelease(wins);
        }
    }
    CFRelease(app);
    return (AXUIElementRef)w;
}

static bool ax_attr_equals(AXUIElementRef e, CFStringRef attr, CFStringRef want) {
    CFTypeRef v = NULL;
    bool eq = false;
    if (AXUIElementCopyAttributeValue(e, attr, &v) == kAXErrorSuccess && v) {
        eq = CFGetTypeID(v) == CFStringGetTypeID() &&
             CFStringCompare((CFStringRef)v, want, 0) == kCFCompareEqualTo;
        CFRelease(v);
    }
    return eq;
}

static bool ax_is_button_labelled(AXUIElementRef e, CFStringRef want) {
    if (!ax_attr_equals(e, kAXRoleAttribute, kAXButtonRole))
        return false;
    return ax_attr_equals(e, kAXDescriptionAttribute, want) ||
           ax_attr_equals(e, kAXTitleAttribute, want);
}

/* Depth-first search; returns a retained element or NULL. */
static AXUIElementRef ax_find_button(AXUIElementRef e, CFStringRef want, int depth) {
    if (depth > VREC_AX_MAX_DEPTH)
        return NULL;
    if (ax_is_button_labelled(e, want)) {
        CFRetain(e);
        return e;
    }
    CFArrayRef kids = NULL;
    if (AXUIElementCopyAttributeValue(e, kAXChildrenAttribute, (CFTypeRef *)&kids) !=
            kAXErrorSuccess ||
        !kids)
        return NULL;
    AXUIElementRef hit = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(kids) && !hit; i++)
        hit = ax_find_button((AXUIElementRef)CFArrayGetValueAtIndex(kids, i), want, depth + 1);
    CFRelease(kids);
    return hit;
}

static AXUIElementRef find_labelled_button(const char *label) {
    AXUIElementRef win = messages_window();
    if (!win)
        return NULL;
    CFStringRef want = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);
    AXUIElementRef hit = want ? ax_find_button(win, want, 0) : NULL;
    if (want)
        CFRelease(want);
    CFRelease(win);
    return hit;
}

static hu_error_t mac_press(void *ctx, const char *label) {
    (void)ctx;
    AXUIElementRef b = find_labelled_button(label);
    if (!b)
        return HU_ERR_NOT_FOUND;
    AXError err = AXUIElementPerformAction(b, kAXPressAction);
    CFRelease(b);
    return err == kAXErrorSuccess ? HU_OK : HU_ERR_IO;
}

static bool mac_wait_label(void *ctx, const char *label, uint32_t timeout_ms) {
    (void)ctx;
    for (uint32_t t = 0; t <= timeout_ms; t += 50) {
        AXUIElementRef b = find_labelled_button(label);
        if (b) {
            CFRelease(b);
            return true;
        }
        usleep(50000);
    }
    return false;
}

static void set_frontmost(pid_t pid) {
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app)
        return;
    (void)AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
    CFRelease(app);
}

static hu_error_t mac_remember_ui(void *ctx) {
    vrec_mac_ctx_t *m = (vrec_mac_ctx_t *)ctx;
    m->front_pid = 0;
    m->win_title[0] = '\0';
    AXUIElementRef sys = AXUIElementCreateSystemWide();
    if (sys) {
        CFTypeRef app = NULL;
        if (AXUIElementCopyAttributeValue(sys, kAXFocusedApplicationAttribute, &app) ==
                kAXErrorSuccess &&
            app) {
            (void)AXUIElementGetPid((AXUIElementRef)app, &m->front_pid);
            CFRelease(app);
        }
        CFRelease(sys);
    }
    AXUIElementRef win = messages_window();
    if (win) {
        CFTypeRef t = NULL;
        if (AXUIElementCopyAttributeValue(win, kAXTitleAttribute, &t) == kAXErrorSuccess && t) {
            if (CFGetTypeID(t) == CFStringGetTypeID())
                (void)CFStringGetCString((CFStringRef)t, m->win_title, sizeof(m->win_title),
                                         kCFStringEncodingUTF8);
            CFRelease(t);
        }
        CFRelease(win);
    }
    return HU_OK;
}

static void mac_restore_ui(void *ctx) {
    vrec_mac_ctx_t *m = (vrec_mac_ctx_t *)ctx;
    /* Best effort: reselect the conversation the user had open when its
     * sidebar entry is an AX button with that title (pinned chats are). */
    bool is_control = strcmp(m->win_title, HU_VREC_LABEL_RECORD) == 0 ||
                      strcmp(m->win_title, HU_VREC_LABEL_STOP) == 0 ||
                      strcmp(m->win_title, HU_VREC_LABEL_SEND) == 0 ||
                      strcmp(m->win_title, HU_VREC_LABEL_CANCEL) == 0;
    if (m->win_title[0] && !is_control && mac_press(ctx, m->win_title) != HU_OK)
        hu_log_info("imessage", NULL, "voice record: previous conversation not reselected");
    if (m->front_pid > 0)
        set_frontmost(m->front_pid);
}

static hu_error_t mac_open_chat(void *ctx, const char *handle, size_t handle_len) {
    (void)ctx;
    char url[320];
    int n = snprintf(url, sizeof(url), "imessage://%.*s", (int)handle_len, handle);
    if (n <= 0 || (size_t)n >= sizeof(url))
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"open", url, NULL};
    hu_run_result_t rr = {0};
    hu_error_t e = hu_process_run(&alloc, argv, NULL, 4096, &rr);
    bool ok = e == HU_OK && rr.success && rr.exit_code == 0;
    hu_run_result_free(&alloc, &rr);
    usleep(300000); /* let Messages switch conversations */
    return ok ? HU_OK : HU_ERR_IO;
}

/* ── AudioQueue playback into BlackHole ────────────────────────────────── */

static void aq_fill(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    vrec_mac_ctx_t *m = (vrec_mac_ctx_t *)ud;
    if (atomic_load(&m->done_reading))
        return;
    UInt32 nbytes = m->buf_bytes;
    UInt32 npk = m->pkts_per_buf;
    OSStatus st = AudioFileReadPacketData(m->file, false, &nbytes, m->pkt_descs, m->next_packet,
                                          &npk, b->mAudioData);
    if (st != noErr || npk == 0) {
        atomic_store(&m->done_reading, 1);
        if (atomic_load(&m->started))
            AudioQueueStop(q, false); /* drain what is queued, then stop */
        return;
    }
    b->mAudioDataByteSize = nbytes;
    AudioQueueEnqueueBuffer(q, b, m->pkt_descs ? npk : 0, m->pkt_descs);
    m->next_packet += npk;
}

static CFStringRef blackhole_uid(void) {
    AudioObjectID id = find_input_device(HU_VREC_BLACKHOLE_NAME);
    if (id == kAudioObjectUnknown)
        return NULL;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef uid = NULL;
    UInt32 sz = (UInt32)sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, (void *)&uid) != noErr)
        return NULL;
    return uid;
}

static void mac_playback_dispose(void *ctx) {
    vrec_mac_ctx_t *m = (vrec_mac_ctx_t *)ctx;
    if (m->queue)
        AudioQueueDispose(m->queue, true);
    if (m->file)
        AudioFileClose(m->file);
    free(m->pkt_descs);
    m->queue = NULL;
    m->file = NULL;
    m->pkt_descs = NULL;
    m->next_packet = 0;
    atomic_store(&m->done_reading, 0);
    atomic_store(&m->started, 0);
}

static hu_error_t mac_playback_prepare(void *ctx, const char *audio_path) {
    vrec_mac_ctx_t *m = (vrec_mac_ctx_t *)ctx;
    mac_playback_dispose(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)audio_path,
                                                           (CFIndex)strlen(audio_path), false);
    if (!url)
        return HU_ERR_IO;
    OSStatus st = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &m->file);
    CFRelease(url);
    if (st != noErr)
        return HU_ERR_IO;
    AudioStreamBasicDescription fmt;
    UInt32 sz = sizeof(fmt);
    if (AudioFileGetProperty(m->file, kAudioFilePropertyDataFormat, &sz, &fmt) != noErr)
        goto fail;
    UInt32 max_pkt = 0;
    sz = sizeof(max_pkt);
    (void)AudioFileGetProperty(m->file, kAudioFilePropertyPacketSizeUpperBound, &sz, &max_pkt);
    m->buf_bytes = max_pkt > VREC_BUF_BYTES ? max_pkt : VREC_BUF_BYTES;
    m->pkts_per_buf = max_pkt ? m->buf_bytes / max_pkt : 1;
    if (fmt.mBytesPerPacket == 0 || fmt.mFramesPerPacket == 0) {
        m->pkt_descs = (AudioStreamPacketDescription *)calloc(m->pkts_per_buf,
                                                              sizeof(AudioStreamPacketDescription));
        if (!m->pkt_descs)
            goto fail;
    }
    double dur = 0.0;
    sz = sizeof(dur);
    (void)AudioFileGetProperty(m->file, kAudioFilePropertyEstimatedDuration, &sz, &dur);
    m->duration_sec = dur;
    if (AudioQueueNewOutput(&fmt, aq_fill, m, NULL, NULL, 0, &m->queue) != noErr)
        goto fail;
    CFStringRef uid = blackhole_uid();
    if (!uid)
        goto fail;
    st = AudioQueueSetProperty(m->queue, kAudioQueueProperty_CurrentDevice, (const void *)&uid,
                               (UInt32)sizeof(CFStringRef));
    CFRelease(uid);
    if (st != noErr)
        goto fail;
    UInt32 cookie_sz = 0;
    if (AudioFileGetPropertyInfo(m->file, kAudioFilePropertyMagicCookieData, &cookie_sz, NULL) ==
            noErr &&
        cookie_sz > 0) {
        void *cookie = malloc(cookie_sz);
        if (cookie) {
            if (AudioFileGetProperty(m->file, kAudioFilePropertyMagicCookieData, &cookie_sz,
                                     cookie) == noErr)
                (void)AudioQueueSetProperty(m->queue, kAudioQueueProperty_MagicCookie, cookie,
                                            cookie_sz);
            free(cookie);
        }
    }
    for (int i = 0; i < VREC_NUM_BUFS; i++) {
        if (AudioQueueAllocateBuffer(m->queue, m->buf_bytes, &m->bufs[i]) != noErr)
            goto fail;
        aq_fill(m, m->queue, m->bufs[i]);
    }
    /* Decode ahead so playback starts the instant Record is live. */
    if (AudioQueuePrime(m->queue, 0, NULL) != noErr)
        goto fail;
    return HU_OK;
fail:
    mac_playback_dispose(ctx);
    return HU_ERR_IO;
}

static hu_error_t mac_playback_run(void *ctx) {
    vrec_mac_ctx_t *m = (vrec_mac_ctx_t *)ctx;
    if (!m->queue)
        return HU_ERR_INVALID_ARGUMENT;
    atomic_store(&m->started, 1);
    if (AudioQueueStart(m->queue, NULL) != noErr)
        return HU_ERR_IO;
    if (atomic_load(&m->done_reading))
        AudioQueueStop(m->queue, false); /* whole clip already enqueued */
    double limit_ms = (m->duration_sec > 0.0 ? m->duration_sec : 60.0) * 1000.0 + 2000.0;
    for (double t = 0.0; t < limit_ms; t += 10.0) {
        UInt32 running = 1, sz = sizeof(running);
        if (AudioQueueGetProperty(m->queue, kAudioQueueProperty_IsRunning, &running, &sz) ==
                noErr &&
            !running && t > 200.0)
            return HU_OK;
        usleep(10000);
    }
    return HU_ERR_IO; /* never finished: treat as a failed recording */
}

/* ── facts, timing, chat.db ────────────────────────────────────────────── */

static hu_error_t mac_gather_facts(void *ctx, const char *real_mic, hu_voice_record_facts_t *out) {
    memset(out, 0, sizeof(*out));
    out->ax_trusted = AXIsProcessTrusted();
    out->messages_running = hu_imessage_messages_pid() > 0;
    out->blackhole_present = find_input_device(HU_VREC_BLACKHOLE_NAME) != kAudioObjectUnknown;
    out->real_mic_configured = real_mic && real_mic[0];
    AudioObjectID mic =
        out->real_mic_configured ? find_input_device(real_mic) : kAudioObjectUnknown;
    out->real_mic_present = mic != kAudioObjectUnknown;
    out->real_mic_busy = out->real_mic_present && dev_running_somewhere(mic);
    char cur[256] = {0};
    out->default_input_is_real_mic = out->real_mic_configured &&
                                     mac_get_input(ctx, cur, sizeof(cur)) == HU_OK &&
                                     strcmp(cur, real_mic) == 0;
    AudioObjectID bh = find_input_device(HU_VREC_BLACKHOLE_NAME);
    out->blackhole_busy = bh != kAudioObjectUnknown && dev_running_somewhere(bh);
    out->user_idle_sec = hid_idle_sec();
    return HU_OK;
}

static void mac_sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    usleep((useconds_t)ms * 1000u);
}

static int64_t mac_max_rowid(void *ctx) {
    (void)ctx;
    return hu_imessage_chatdb_max_rowid();
}

static bool mac_audio_row_after(void *ctx, const char *handle, size_t handle_len,
                                int64_t after_rowid, uint32_t timeout_ms) {
    (void)ctx;
    for (uint32_t t = 0; t <= timeout_ms; t += 250) {
        if (hu_imessage_chatdb_audio_from_me_after(handle, handle_len, after_rowid))
            return true;
        usleep(250000);
    }
    return false;
}

static hu_error_t mac_chat_title(void *ctx, char *buf, size_t cap) {
    (void)ctx;
    if (!buf || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    buf[0] = '\0';
    AXUIElementRef win = messages_window();
    if (!win)
        return HU_ERR_NOT_FOUND;
    CFTypeRef t = NULL;
    bool ok = false;
    if (AXUIElementCopyAttributeValue(win, kAXTitleAttribute, &t) == kAXErrorSuccess && t) {
        ok = CFGetTypeID(t) == CFStringGetTypeID() &&
             CFStringGetCString((CFStringRef)t, buf, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(t);
    }
    CFRelease(win);
    return ok ? HU_OK : HU_ERR_NOT_FOUND;
}

/* Messages resolves the display name exactly as it titles the conversation
 * window. The handle was validated (phone/email characters only), so it can
 * be quoted into the script verbatim. */
static hu_error_t mac_expected_title(void *ctx, const char *handle, size_t handle_len, char *buf,
                                     size_t cap) {
    (void)ctx;
    if (!buf || cap == 0 || !hu_voice_record_handle_ok(handle, handle_len))
        return HU_ERR_INVALID_ARGUMENT;
    buf[0] = '\0';
    char script[384];
    int n = snprintf(script, sizeof(script),
                     "tell application \"Messages\" to get name of first participant whose "
                     "handle is \"%.*s\"",
                     (int)handle_len, handle);
    if (n <= 0 || (size_t)n >= sizeof(script))
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"osascript", "-e", script, NULL};
    hu_run_result_t rr = {0};
    hu_error_t e = hu_process_run_with_timeout(&alloc, argv, NULL, 4096, 5, &rr);
    bool ok = e == HU_OK && rr.success && rr.exit_code == 0 && rr.stdout_buf && rr.stdout_len;
    if (ok) {
        size_t len = rr.stdout_len;
        while (len > 0 && (rr.stdout_buf[len - 1] == '\n' || rr.stdout_buf[len - 1] == '\r'))
            len--;
        ok = len > 0 && len < cap;
        if (ok) {
            memcpy(buf, rr.stdout_buf, len);
            buf[len] = '\0';
        }
    }
    hu_run_result_free(&alloc, &rr);
    return ok ? HU_OK : HU_ERR_NOT_FOUND;
}

static double mac_idle_sec(void *ctx) {
    (void)ctx;
    return hid_idle_sec();
}

static uint64_t mac_now_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* One recording at a time: a mutex for this process, an flock for the daemon
 * and `human voice record-send` running side by side. */
static pthread_mutex_t g_vrec_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_vrec_lock_fd = -1;

static bool mac_try_lock(void *ctx) {
    (void)ctx;
    if (pthread_mutex_trylock(&g_vrec_mu) != 0)
        return false;
    char path[512];
    int n = hu_paths_state(path, sizeof(path), "voice_record.lock");
    int fd = (n > 0 && (size_t)n < sizeof(path)) ? open(path, O_CREAT | O_RDWR, 0600) : -1;
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0)
            close(fd);
        pthread_mutex_unlock(&g_vrec_mu);
        return false;
    }
    g_vrec_lock_fd = fd;
    return true;
}

static void mac_unlock(void *ctx) {
    (void)ctx;
    if (g_vrec_lock_fd >= 0) {
        flock(g_vrec_lock_fd, LOCK_UN);
        close(g_vrec_lock_fd);
        g_vrec_lock_fd = -1;
    }
    pthread_mutex_unlock(&g_vrec_mu);
}

static const hu_voice_record_port_t k_mac_port = {
    &g_vrec,          mac_gather_facts,     mac_set_input, mac_get_input,  mac_remember_ui,
    mac_restore_ui,   mac_open_chat,        mac_press,     mac_wait_label, mac_playback_prepare,
    mac_playback_run, mac_playback_dispose, mac_sleep_ms,  mac_max_rowid,  mac_audio_row_after,
    mac_chat_title,   mac_expected_title,   mac_idle_sec,  mac_now_ms,     mac_try_lock,
    mac_unlock,
};

const hu_voice_record_port_t *hu_voice_record_macos_port(void) {
    return &k_mac_port;
}

double hu_voice_record_idle_seconds(void) {
    return hid_idle_sec();
}

bool hu_voice_record_ax_trusted(void) {
    return AXIsProcessTrusted();
}

#else /* test or non-Apple: preflight always blocks; every member is safe */

static hu_error_t stub_facts(void *c, const char *m, hu_voice_record_facts_t *o) {
    (void)c;
    (void)m;
    if (o)
        memset(o, 0, sizeof(*o));
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t stub_set_input(void *c, const char *d) {
    (void)c;
    (void)d;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t stub_get_input(void *c, char *b, size_t n) {
    (void)c;
    if (b && n)
        b[0] = '\0';
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t stub_ctx_only(void *c) {
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
}
static void stub_void(void *c) {
    (void)c;
}
static hu_error_t stub_open(void *c, const char *h, size_t n) {
    (void)c;
    (void)h;
    (void)n;
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t stub_press(void *c, const char *l) {
    (void)c;
    (void)l;
    return HU_ERR_NOT_SUPPORTED;
}
static bool stub_wait(void *c, const char *l, uint32_t t) {
    (void)c;
    (void)l;
    (void)t;
    return false;
}
static hu_error_t stub_prepare(void *c, const char *p) {
    (void)c;
    (void)p;
    return HU_ERR_NOT_SUPPORTED;
}
static void stub_sleep(void *c, uint32_t ms) {
    (void)c;
    (void)ms;
}
static int64_t stub_rowid(void *c) {
    (void)c;
    return -1;
}
static bool stub_row(void *c, const char *h, size_t n, int64_t a, uint32_t t) {
    (void)c;
    (void)h;
    (void)n;
    (void)a;
    (void)t;
    return false;
}

static hu_error_t stub_title(void *c, char *b, size_t n) {
    (void)c;
    if (b && n)
        b[0] = '\0';
    return HU_ERR_NOT_SUPPORTED;
}
static hu_error_t stub_expected(void *c, const char *h, size_t hl, char *b, size_t n) {
    (void)h;
    (void)hl;
    return stub_title(c, b, n);
}
static double stub_idle(void *c) {
    (void)c;
    return 0.0;
}
static uint64_t stub_now(void *c) {
    (void)c;
    return 0;
}
static bool stub_lock(void *c) {
    (void)c;
    return true; /* preflight blocks right after; unlock follows */
}

static const hu_voice_record_port_t k_stub_port = {
    NULL,       stub_facts, stub_set_input, stub_get_input, stub_ctx_only, stub_void,  stub_open,
    stub_press, stub_wait,  stub_prepare,   stub_ctx_only,  stub_void,     stub_sleep, stub_rowid,
    stub_row,   stub_title, stub_expected,  stub_idle,      stub_now,      stub_lock,  stub_void,
};

const hu_voice_record_port_t *hu_voice_record_macos_port(void) {
    return &k_stub_port;
}

double hu_voice_record_idle_seconds(void) {
    return 0.0; /* unknown reads as "active": nothing waits on it here */
}

bool hu_voice_record_ax_trusted(void) {
    return true; /* nothing to grant here; never alarm */
}

#endif
