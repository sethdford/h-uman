/*
 * US-43.3 — iMessage non-allowlisted courtesy reply.
 *
 * Pure predicate + tiny I/O wrappers for a single polite courtesy reply per
 * unknown sender per 24h (per-handle), gated by a 50/day aggregate cap, with
 * fail-CLOSED behavior on any dedup-log I/O ambiguity.
 *
 * Design contract is in `include/human/channels/imessage_courtesy.h` and the
 * full risk/test treatment is in
 * `sprints/sprint-43/designs/US-43.3.md`.
 *
 * Security predicate extraction pattern: see
 * `.claude/rules/security-predicate-extraction.md`. The predicate is the
 * security decision; everything else (log scan, ring) is plumbing.
 */
#include "human/channels/imessage_courtesy.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Pure predicate (AC-43.3.1 / .2 / .3 / .4)
 * ─────────────────────────────────────────────────────────────────────────── */

bool hu_imessage_should_courtesy_reply(bool handle_in_allowlist, double hours_since_last_reply,
                                       int aggregate_today, bool dedup_io_ok) {
    /* Defense-in-depth: callers MUST only invoke for non-allowlisted senders,
     * but if a bug routes an allowed handle here, refuse — never send when
     * the sender is on the allowlist (US-43.3 covers UNKNOWN senders). */
    if (handle_in_allowlist) {
        return false;
    }
    /* AC-43.3.4: any dedup-log uncertainty fails CLOSED. */
    if (!dedup_io_ok) {
        return false;
    }
    /* AC-43.3.3: 50/day aggregate cap. >= protects against off-by-one if the
     * caller pre-counts the current pending reply. */
    if (aggregate_today >= HU_IMESSAGE_COURTESY_AGGREGATE_PER_DAY) {
        return false;
    }
    /* AC-43.3.2: per-handle 24h dedup. NaN / negative values fail CLOSED. */
    if (!(hours_since_last_reply >= HU_IMESSAGE_COURTESY_PER_HANDLE_HOURS)) {
        return false;
    }
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Handle-shaped name stripping (AC-43.3.5)
 * ─────────────────────────────────────────────────────────────────────────── */

static bool name_looks_handle_shaped(const char *s) {
    if (!s || !*s) {
        return true; /* empty ⇒ no real name; replace with "there" */
    }
    /* Phone-shaped prefix or any @ marker (email-like) ⇒ strip. */
    if (s[0] == '+') {
        return true;
    }
    if (strchr(s, '@') != NULL) {
        return true;
    }
    /* Bare phone glyphs only? "+1 (415) 555-0100" minus the "+" still scans as
     * digits, spaces, parens, dashes — strip. */
    bool any_alpha = false;
    bool any_digit = false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalpha(c)) {
            any_alpha = true;
        } else if (isdigit(c)) {
            any_digit = true;
        } else if (c == ' ' || c == '(' || c == ')' || c == '-' || c == '+' || c == '.') {
            /* phone punctuation, fine */
        } else {
            /* anything else (curly quote, etc.) is not handle-shaped — let it through */
            return false;
        }
    }
    if (any_digit && !any_alpha) {
        return true;
    }
    return false;
}

size_t hu_imessage_courtesy_sanitize_name(const char *in, char *out, size_t out_cap) {
    if (!out || out_cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (name_looks_handle_shaped(in)) {
        const char *safe = "there";
        size_t n = strlen(safe);
        if (n >= out_cap) {
            n = out_cap - 1;
        }
        memcpy(out, safe, n);
        out[n] = '\0';
        return n;
    }
    /* Real-looking name. Copy verbatim. If the name embeds a phone-shaped
     * substring (e.g. "Bob +14155550100"), strip the substring and trim. */
    size_t in_len = strlen(in);
    size_t w = 0;
    bool in_phone_run = false;
    for (size_t i = 0; i < in_len && w + 1 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        bool is_phone_char = (c == '+' || c == '(' || c == ')' || c == '-' || isdigit(c));
        if (is_phone_char) {
            in_phone_run = true;
            continue;
        }
        if (in_phone_run) {
            in_phone_run = false;
            /* collapse the stripped run into a single space if we already have
             * non-space content */
            if (w > 0 && out[w - 1] != ' ' && c != ' ' && w + 1 < out_cap) {
                out[w++] = ' ';
            }
        }
        out[w++] = (char)c;
    }
    /* Trim trailing whitespace. */
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
        w--;
    }
    out[w] = '\0';
    /* If everything got stripped, default to "there". */
    if (w == 0) {
        const char *safe = "there";
        size_t n = strlen(safe);
        if (n >= out_cap) {
            n = out_cap - 1;
        }
        memcpy(out, safe, n);
        out[n] = '\0';
        return n;
    }
    return w;
}

size_t hu_imessage_courtesy_compose_reply(const char *sanitized_name, char *out, size_t out_cap) {
    if (!out || out_cap == 0) {
        return 0;
    }
    const char *who = (sanitized_name && *sanitized_name) ? sanitized_name : "there";
    int n = snprintf(out, out_cap,
                     "Hi %s — this is an AI assistant replying on behalf of "
                     "its user. You're not on the allowlist, so I won't pass "
                     "your message along automatically. If you know the "
                     "person, please reach them another way.",
                     who);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    if ((size_t)n >= out_cap) {
        return out_cap - 1;
    }
    return (size_t)n;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Dedup log: open + flock(LOCK_EX) + scan + optional append + fsync + unlock
 *
 * Format: one record per line, "<handle>\t<epoch_secs>\n". UTF-8 safe. Records
 * older than the per-handle window are kept (cheap to scan) but excluded from
 * the per-handle hours_since computation. The aggregate counter uses the UTC
 * day bucket `floor(epoch_secs/86400)`.
 * ─────────────────────────────────────────────────────────────────────────── */

static int resolve_log_path(char *path, size_t path_cap) {
    const char *override = getenv("HU_IMESSAGE_COURTESY_LOG_PATH");
    if (override && *override) {
        size_t n = strlen(override);
        if (n + 1 > path_cap) {
            return -1;
        }
        memcpy(path, override, n + 1);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return -1;
    }
    int n = snprintf(path, path_cap, "%s/.human", home);
    if (n < 0 || (size_t)n >= path_cap) {
        return -1;
    }
    /* Best-effort mkdir of ~/.human (mode 0700); ignore EEXIST. */
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        /* If we can't create the dir we still try the open() below; it will
         * fail and that's fine — caller sees io_ok=false and fails CLOSED. */
    }
    n = snprintf(path, path_cap, "%s/.human/imessage_courtesy.log", home);
    if (n < 0 || (size_t)n >= path_cap) {
        return -1;
    }
    return 0;
}

/* Scan a buffer of newline-terminated "<handle>\t<epoch>\n" records. Updates
 * `state` in place. Lines that don't match are silently skipped (forward-compat
 * with future record types). */
static void scan_records(const char *buf, size_t len, const char *handle, int64_t now_epoch,
                         hu_imessage_courtesy_state_t *state) {
    const int64_t day_now = now_epoch / 86400;
    int64_t latest_for_handle = -1;
    int aggregate = 0;
    size_t handle_len = handle ? strlen(handle) : 0;
    size_t i = 0;
    while (i < len) {
        /* find newline */
        size_t line_start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t line_len = i - line_start;
        if (i < len) {
            i++; /* skip \n */
        }
        if (line_len == 0) {
            continue;
        }
        /* split on \t */
        const char *line = buf + line_start;
        const char *tab = memchr(line, '\t', line_len);
        if (!tab) {
            continue;
        }
        size_t h_len = (size_t)(tab - line);
        const char *ts_str = tab + 1;
        size_t ts_len = line_len - h_len - 1;
        if (ts_len == 0 || ts_len > 32) {
            continue;
        }
        char ts_buf[33];
        memcpy(ts_buf, ts_str, ts_len);
        ts_buf[ts_len] = '\0';
        char *endp = NULL;
        long long ts_ll = strtoll(ts_buf, &endp, 10);
        if (!endp || *endp != '\0' || ts_ll < 0) {
            continue;
        }
        int64_t ts = (int64_t)ts_ll;
        /* Aggregate-today: same UTC day bucket. */
        if (ts / 86400 == day_now) {
            aggregate++;
        }
        /* Per-handle: track latest matching epoch (case-insensitive compare —
         * iMessage handles can vary in normalization). */
        if (handle_len > 0 && h_len == handle_len && strncasecmp(line, handle, h_len) == 0) {
            if (ts > latest_for_handle) {
                latest_for_handle = ts;
            }
        }
    }
    state->aggregate_today = aggregate;
    if (latest_for_handle < 0) {
        /* No prior record — clamp to a large positive value so the predicate's
         * `hours_since >= 24` check succeeds for fresh senders. */
        state->hours_since_last = 1e9;
    } else {
        double delta = (double)(now_epoch - latest_for_handle);
        if (delta < 0.0) {
            delta = 0.0;
        }
        state->hours_since_last = delta / 3600.0;
    }
}

hu_error_t hu_imessage_courtesy_eval_and_record(const char *handle, int64_t now_epoch,
                                                bool record_after, bool *recorded_out,
                                                hu_imessage_courtesy_state_t *state_out) {
    if (!state_out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    state_out->io_ok = false;
    state_out->hours_since_last = 0.0;
    state_out->aggregate_today = 0;
    if (recorded_out) {
        *recorded_out = false;
    }

    char path[512];
    if (resolve_log_path(path, sizeof(path)) != 0) {
        /* No HOME and no override — treat as fail-CLOSED, signal upstream. */
        return HU_OK;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        /* Permission denied / read-only fs / etc. — fail CLOSED. */
        return HU_OK;
    }

    /* Acquire the exclusive lock — held across the entire read-eval-write
     * sequence so no concurrent process can sneak a record between our check
     * and our append. */
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return HU_OK; /* lock failure ⇒ fail CLOSED */
    }

    /* Read entire file. Cap to 1 MiB — far above the 50/day cap so a real log
     * is always much smaller. If a log somehow grows past that, treat as
     * I/O-uncertain (defense against a corrupted or attacker-grown log). */
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return HU_OK;
    }
    if ((size_t)st.st_size > (size_t)(1024 * 1024)) {
        flock(fd, LOCK_UN);
        close(fd);
        return HU_OK;
    }
    size_t fsize = (size_t)st.st_size;
    char *buf = NULL;
    if (fsize > 0) {
        buf = (char *)malloc(fsize);
        if (!buf) {
            flock(fd, LOCK_UN);
            close(fd);
            return HU_OK;
        }
        size_t off = 0;
        while (off < fsize) {
            ssize_t r = read(fd, buf + off, fsize - off);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                free(buf);
                flock(fd, LOCK_UN);
                close(fd);
                return HU_OK; /* fail CLOSED */
            }
            if (r == 0) {
                break;
            }
            off += (size_t)r;
        }
        fsize = off;
    }

    scan_records(buf ? buf : "", fsize, handle, now_epoch, state_out);
    state_out->io_ok = true;
    free(buf);
    buf = NULL;

    /* Decide whether to record. Use the same predicate the caller will use;
     * record only when we would actually send. The caller is responsible for
     * passing `handle_in_allowlist=false`. */
    bool would_send = hu_imessage_should_courtesy_reply(false, state_out->hours_since_last,
                                                        state_out->aggregate_today, true);

    if (record_after && would_send && handle && *handle) {
        /* Append "<handle>\t<now>\n". */
        char line[HU_IMESSAGE_COURTESY_HANDLE_MAX + 64];
        int n = snprintf(line, sizeof(line), "%s\t%lld\n", handle, (long long)now_epoch);
        if (n > 0 && (size_t)n < sizeof(line)) {
            if (lseek(fd, 0, SEEK_END) >= 0) {
                ssize_t w = write(fd, line, (size_t)n);
                if (w == (ssize_t)n) {
                    /* Best-effort fsync; if it fails the rest of the
                     * pipeline still respects in-memory state — but to honor
                     * "fail CLOSED on dedup I/O ambiguity" we degrade io_ok
                     * to false so the predicate refuses. */
                    if (fsync(fd) != 0) {
                        state_out->io_ok = false;
                    } else {
                        if (recorded_out) {
                            *recorded_out = true;
                        }
                        /* Bump the aggregate so the predicate sees the new
                         * count consistently in the same call. */
                        state_out->aggregate_today++;
                        state_out->hours_since_last = 0.0;
                    }
                } else {
                    state_out->io_ok = false;
                }
            } else {
                state_out->io_ok = false;
            }
        } else {
            state_out->io_ok = false;
        }
    }

    flock(fd, LOCK_UN);
    close(fd);
    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Pending-courtesy ring (drained by agent loop, not poll thread)
 * ─────────────────────────────────────────────────────────────────────────── */

void hu_imessage_courtesy_ring_init(hu_imessage_courtesy_ring_t *ring) {
    if (!ring) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
}

bool hu_imessage_courtesy_ring_enqueue(hu_imessage_courtesy_ring_t *ring, const char *handle,
                                       const char *body) {
    if (!ring || !handle || !body) {
        return false;
    }
    if (ring->count >= HU_IMESSAGE_COURTESY_RING_CAPACITY) {
        ring->refused_enqueues++;
        return false;
    }
    hu_imessage_courtesy_pending_t *slot = &ring->slots[ring->head];
    size_t hn = strlen(handle);
    if (hn >= sizeof(slot->handle)) {
        hn = sizeof(slot->handle) - 1;
    }
    memcpy(slot->handle, handle, hn);
    slot->handle[hn] = '\0';
    size_t bn = strlen(body);
    if (bn >= sizeof(slot->body)) {
        bn = sizeof(slot->body) - 1;
    }
    memcpy(slot->body, body, bn);
    slot->body[bn] = '\0';
    ring->head = (ring->head + 1) % HU_IMESSAGE_COURTESY_RING_CAPACITY;
    ring->count++;
    return true;
}

bool hu_imessage_courtesy_ring_drain_one(hu_imessage_courtesy_ring_t *ring,
                                         hu_imessage_courtesy_pending_t *out) {
    if (!ring || !out) {
        return false;
    }
    if (ring->count == 0) {
        return false;
    }
    hu_imessage_courtesy_pending_t *slot = &ring->slots[ring->tail];
    memcpy(out, slot, sizeof(*out));
    memset(slot, 0, sizeof(*slot));
    ring->tail = (ring->tail + 1) % HU_IMESSAGE_COURTESY_RING_CAPACITY;
    ring->count--;
    return true;
}

size_t hu_imessage_courtesy_ring_count(const hu_imessage_courtesy_ring_t *ring) {
    return ring ? ring->count : 0;
}

uint64_t hu_imessage_courtesy_ring_refused(const hu_imessage_courtesy_ring_t *ring) {
    return ring ? ring->refused_enqueues : 0;
}
