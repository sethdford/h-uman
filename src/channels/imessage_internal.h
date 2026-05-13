#ifndef HU_IMESSAGE_INTERNAL_H
#define HU_IMESSAGE_INTERNAL_H

/*
 * Cross-module signatures for the carved-out iMessage modules.
 *
 * NOT part of the public API at include/human/channels/imessage.h. Consumers
 * of iMessage from outside src/channels/ continue to use only the public
 * header. This file exists solely so the (still-being-carved) imessage*.c
 * modules can call each other without making everything global.
 *
 * Current contents:
 *   - AX (Accessibility) module — src/channels/imessage_ax.c
 *
 * Future modules per docs/plans/2026-05-12-imessage-shape-refactor.md will
 * add their own internal signatures here.
 *
 * Visibility note: until the build system gains a `-fvisibility=hidden`
 * default for libhuman_core, these symbols are still externally linkable;
 * the contract is by header inclusion + naming convention. Treat any
 * symbol declared in this header as INTERNAL — do not call from outside
 * src/channels/imessage*.c.
 */

#include <stdbool.h>
#include <stddef.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)

/* AX (Accessibility) module — src/channels/imessage_ax.c.
 *
 * Apple-only. On non-Apple or test builds these functions are not defined;
 * callers MUST guard each call site with the same
 *   #if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
 * that gates the module itself. Declarations are inside that guard here
 * to make link-time mistakes loud (undefined symbol) rather than silent.
 */

/* Open a chat in Messages.app for the given recipient (phone / email /
 * group GUID). Activates Messages.app if it isn't focused. Idempotent. */
void ax_open_conversation(const char *recipient, size_t recipient_len);

/* Start the iMessage typing indicator for `target` by focusing the
 * Messages.app compose field and typing+clearing a placeholder character.
 * Returns true on success; false if AX permission is missing, Messages.app
 * isn't running, or the compose field couldn't be found. */
bool ax_start_typing(const char *target, size_t target_len);

/* Stop the iMessage typing indicator by clearing the compose field. */
bool ax_stop_typing(void);

#ifdef HU_IMESSAGE_TAPBACK_ENABLED
/* Send a tapback (love / like / dislike / laugh / emphasize / question)
 * to a message identified by a content prefix + row offset within the
 * current chat. Requires HU_IMESSAGE_TAPBACK_ENABLED at build time
 * because the AX context-menu walk is macOS-version-fragile. */
bool ax_tapback(const char *content_prefix, int row_offset, const char *tapback_label);
#endif

#endif /* !HU_IS_TEST && __APPLE__ && __MACH__ */

#endif /* HU_IMESSAGE_INTERNAL_H */
