/*
 * imessage_react.c — Tapback reaction dispatch (3-tier fallback).
 *
 * Step 4 of the iMessage shape refactor — see
 * docs/plans/2026-05-12-imessage-shape-refactor.md.
 *
 * Sending a tapback (love / like / dislike / laugh / emphasize / question)
 * has no public Apple API. We try three tiers in order:
 *
 *   Tier 1 — `imsg react` CLI (when use_imsg_cli + the binary is on PATH).
 *            Most robust; only requires the imsg CLI itself, no AX walk.
 *   Tier 2 — Native AX (Accessibility framework). Walks the Messages.app
 *            UI tree to the target row and performs the AX tapback action.
 *            Implementation lives in src/channels/imessage_ax.c; we call
 *            ax_open_conversation + ax_tapback. Gated behind
 *            HU_IMESSAGE_TAPBACK_ENABLED because the AX context-menu
 *            walk is macOS-version-fragile.
 *   Tier 3 — JXA subprocess (osascript). The legacy path; brittle since
 *            macOS Sequoia's keystroke restrictions.
 *
 * In the !HU_IMESSAGE_TAPBACK_ENABLED build, only Tier 1 is available;
 * the AX and JXA paths return HU_ERR_NOT_SUPPORTED.
 */

#include "imessage_internal.h"

#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/process_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

hu_error_t imessage_react(void *ctx, const char *target, size_t target_len, int64_t message_id,
                          hu_reaction_type_t reaction) {
    (void)target;
    (void)target_len;
    (void)message_id;
#if HU_IS_TEST
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->last_reaction = reaction;
    c->last_reaction_message_id = message_id;
    return HU_OK;
#else
#if !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)reaction;
    return HU_ERR_NOT_SUPPORTED;
#elif !defined(HU_IMESSAGE_TAPBACK_ENABLED)
    {
        hu_imessage_ctx_t *c_try = (hu_imessage_ctx_t *)ctx;
        if (c_try && c_try->use_imsg_cli && imsg_cli_available(c_try) &&
            imsg_try_react(c_try, message_id, reaction))
            return HU_OK;
        (void)target;
        (void)target_len;
        if (getenv("HU_DEBUG"))
            hu_log_info("imessage", NULL,
                        "tapback: no imsg CLI and JXA disabled "
                        "(HU_IMESSAGE_TAPBACK_ENABLED=OFF)");
        return HU_ERR_NOT_SUPPORTED;
    }
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    const char *tapback_ax = imessage_reaction_to_ax_action_prefix(reaction);
    if (!tapback_ax)
        return HU_ERR_INVALID_ARGUMENT;
    if (!target || target_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (c->use_imsg_cli && imsg_cli_available(c) && imsg_try_react(c, message_id, reaction))
        return HU_OK;

    /* Tier 2: Native AX tapback — uses our process's Accessibility permission
     * directly, bypasses System Events keystroke restriction. */
    /* Fetch raw message text for AX menu-item matching. Intentionally does NOT
     * apply balloon_label or effect_name — tapback needs the original text that
     * the Messages UI displays, not decorated agent-facing labels. */
    char content_buf[256];
    size_t content_len = 0;
    int row_offset = -1; /* messages after target in same chat; -1 = unknown */
#if defined(HU_ENABLE_SQLITE)
    if (message_id > 0) {
        const char *home_env = getenv("HOME");
        if (home_env) {
            char db_path[512];
            int dn = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home_env);
            if (dn > 0 && (size_t)dn < sizeof(db_path)) {
                sqlite3 *db = NULL;
                if (imessage_open_chatdb(db_path, &db) == SQLITE_OK) {
                    sqlite3_stmt *stmt = NULL;
                    if (sqlite3_prepare_v2(
                            db, "SELECT text, attributedBody FROM message WHERE ROWID = ?", -1,
                            &stmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(stmt, 1, message_id);
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char *text = (const char *)sqlite3_column_text(stmt, 0);
                            if (!text || text[0] == '\0') {
                                const unsigned char *ab = sqlite3_column_blob(stmt, 1);
                                int ab_len = sqlite3_column_bytes(stmt, 1);
                                if (ab && ab_len > 0) {
                                    content_len = hu_imessage_extract_attributed_body(
                                        ab, (size_t)ab_len, content_buf, sizeof(content_buf));
                                }
                            } else {
                                size_t len = strlen(text);
                                if (len >= sizeof(content_buf))
                                    len = sizeof(content_buf) - 1;
                                memcpy(content_buf, text, len);
                                content_buf[len] = '\0';
                                content_len = len;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                    /* Row offset: count non-tapback messages after this one in the same chat.
                     * Gives us a reliable index from the bottom of the transcript view. */
                    sqlite3_stmt *off_stmt = NULL;
                    const char *off_sql = "SELECT COUNT(*) FROM message m "
                                          "JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
                                          "WHERE cmj.chat_id = ("
                                          "  SELECT cmj2.chat_id FROM chat_message_join cmj2 "
                                          "  WHERE cmj2.message_id = ?1 LIMIT 1"
                                          ") AND m.ROWID > ?1 AND m.associated_message_type = 0";
                    if (sqlite3_prepare_v2(db, off_sql, -1, &off_stmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(off_stmt, 1, message_id);
                        if (sqlite3_step(off_stmt) == SQLITE_ROW)
                            row_offset = sqlite3_column_int(off_stmt, 0);
                        sqlite3_finalize(off_stmt);
                    }
                    sqlite3_close(db);
                }
            }
        }
    }
#endif
    (void)message_id;

    /* Try native AX tapback first (no subprocess, no keystroke restriction).
     * Must open the conversation in Messages so it's in the AX tree. */
    if (target && target_len > 0)
        ax_open_conversation(target, target_len);
    if (ax_tapback(content_len > 0 ? content_buf : NULL, row_offset, tapback_ax)) {
        hu_log_info("imessage", NULL, "tapback sent via native AX");
        return HU_OK;
    }

    /* Fall back to JXA subprocess (legacy, may fail on Sequoia+). */
    /* Escape content_prefix and tapback_ax for JavaScript string literals. */
    size_t esc_cap = (content_len + strlen(tapback_ax)) * 2 + 64;
    if (esc_cap > 2048)
        esc_cap = 2048;
    char *content_esc = (char *)c->alloc->alloc(c->alloc->ctx, esc_cap);
    if (!content_esc)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < content_len && j + 2 < esc_cap; i++) {
        if (content_buf[i] == '\\' || content_buf[i] == '"') {
            content_esc[j++] = '\\';
            content_esc[j++] = content_buf[i];
        } else if (content_buf[i] == '\n') {
            content_esc[j++] = '\\';
            content_esc[j++] = 'n';
        } else {
            content_esc[j++] = content_buf[i];
        }
    }
    content_esc[j] = '\0';
    size_t content_esc_len = j;

    size_t tapback_esc_cap = strlen(tapback_ax) * 2 + 16;
    char *tapback_esc = (char *)c->alloc->alloc(c->alloc->ctx, tapback_esc_cap);
    if (!tapback_esc) {
        c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    j = 0;
    for (size_t i = 0; tapback_ax[i] && j + 2 < tapback_esc_cap; i++) {
        if (tapback_ax[i] == '\\' || tapback_ax[i] == '"') {
            tapback_esc[j++] = '\\';
            tapback_esc[j++] = tapback_ax[i];
        } else {
            tapback_esc[j++] = tapback_ax[i];
        }
    }
    tapback_esc[j] = '\0';

    /*
     * JXA script: activate Messages, find row by offset or content, AXShowMenu, select tapback.
     * - rowOffset: messages after target in the transcript (-1 = unknown, use content match)
     * - contentPrefix: substring to match in table row (empty = use last row)
     * - tapbackAx: AX menu label (Love, Like, Ha Ha, etc.)
     * Requires accessibility permissions; UI hierarchy varies by macOS version.
     */
    static const char *script_tpl =
        "ObjC.import(\"stdlib\");"
        "var rowOff=%d;var contentPrefix=\"%s\";var tapbackAx=\"%s\";"
        "try{"
        "var M=Application(\"Messages\");M.activate();delay(0.5);"
        "var SE=Application(\"System Events\");var p=SE.processes[\"Messages\"];"
        "if(!p||!p.exists()){$.exit(1);}"
        "var w=p.windows();if(!w||w.length===0){$.exit(1);}"
        "var win=w[0];var t=win.tables();"
        "if(!t||t.length===0){$.exit(1);}"
        "var rows=t[t.length-1].rows();"
        "if(!rows||rows.length===0){$.exit(1);}"
        "var r=null;"
        /* Try row-offset first (most reliable when available). */
        "if(rowOff>=0&&rowOff<rows.length){"
        "var idx=rows.length-1-rowOff;"
        "var cand=rows[idx];var cv=\"\";"
        "try{if(cand.attributes&&cand.attributes.AXValue!==undefined)"
        "{cv=String(cand.attributes.AXValue);}}catch(e){}"
        "if(!contentPrefix||contentPrefix.length===0||"
        "(cv&&cv.indexOf(contentPrefix)!==-1)){r=cand;}"
        "}"
        /* Fall back to content-prefix scan from bottom. */
        "if(!r&&contentPrefix&&contentPrefix.length>0){"
        "for(var i=rows.length-1;i>=0;i--){"
        "var row=rows[i];var val=\"\";"
        "try{if(row.attributes&&row.attributes.AXValue!==undefined){val=String(row.attributes."
        "AXValue);}}catch(e){}"
        "try{if(!val&&row.cells&&row.cells.length>0){var "
        "c0=row.cells[0];if(c0.attributes&&c0.attributes.AXValue!==undefined){val=String(c0."
        "attributes.AXValue);}}}catch(e){}"
        "if(val&&val.indexOf(contentPrefix)!==-1){r=row;break;}"
        "}}"
        /* Last resort: use the most recent row. */
        "if(!r){r=rows[rows.length-1];}"
        "if(r.actions&&r.actions.AXShowMenu){r.actions.AXShowMenu.perform();}"
        "delay(0.5);"
        "var ctxMenus=r.menus?r.menus():[];"
        "if(ctxMenus&&ctxMenus.length>0){"
        "var items=ctxMenus[0].menuItems?ctxMenus[0].menuItems():[];"
        "for(var ii=0;ii<items.length;ii++){"
        "var it=items[ii];var title=(it.title?it.title():\"\")+\"\";"
        "if(title.indexOf(\"Tapback\")!==-1){it.click();delay(0.3);"
        "var sub=it.menus?it.menus():[];"
        "if(sub&&sub.length>0){var subItems=sub[0].menuItems?sub[0].menuItems():[];"
        "for(var si=0;si<subItems.length;si++){var "
        "st=(subItems[si].title?subItems[si].title():\"\")+\"\";"
        "if(st===tapbackAx){subItems[si].click();break;}}"
        "}break;}"
        "}}"
        "}"
        "$.exit(0);"
        "}catch(e){$.exit(1);}";

    size_t script_cap = 2560 + content_esc_len + strlen(tapback_esc);
    char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
    if (!script) {
        c->alloc->free(c->alloc->ctx, tapback_esc, tapback_esc_cap);
        c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(script, script_cap, script_tpl, row_offset, content_esc, tapback_esc);
    c->alloc->free(c->alloc->ctx, tapback_esc, tapback_esc_cap);
    c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
    if (n < 0 || (size_t)n >= script_cap) {
        c->alloc->free(c->alloc->ctx, script, script_cap);
        return HU_ERR_INTERNAL;
    }

    const char *argv[] = {"osascript", "-l", "JavaScript", "-e", script, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_with_timeout(c->alloc, argv, NULL, 65536, 15, &result);
    c->alloc->free(c->alloc->ctx, script, script_cap);
    if (err != HU_OK) {
        hu_log_error("imessage", NULL, "tapback osascript failed: hu_process_run err=%s",
                     hu_error_string(err));
        hu_run_result_free(c->alloc, &result);
        return HU_ERR_NOT_SUPPORTED;
    }
    int exit_code = result.exit_code;
    bool ok = result.success && exit_code == 0;
    if (!ok) {
        hu_log_error("imessage", NULL, "tapback JXA failed: exit=%d stdout=%.*s stderr=%.*s",
                     exit_code,
                     (int)(result.stdout_buf && result.stdout_len > 0
                               ? (result.stdout_len < 200 ? result.stdout_len : 200)
                               : 0),
                     result.stdout_buf ? result.stdout_buf : "",
                     (int)(result.stderr_buf && result.stderr_len > 0
                               ? (result.stderr_len < 200 ? result.stderr_len : 200)
                               : 0),
                     result.stderr_buf ? result.stderr_buf : "");
    }
    hu_run_result_free(c->alloc, &result);
    if (!ok)
        return HU_ERR_NOT_SUPPORTED;
    return HU_OK;
#endif
#endif
}
