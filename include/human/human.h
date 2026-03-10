#ifndef HU_HUMAN_H
#define HU_HUMAN_H

/**
 * Human — autonomous AI assistant runtime (C11).
 * Vtable interface headers.
 *
 * Include this umbrella header to get all public types and interfaces.
 */

/* Core types (Phase 1) */
#include "core/allocator.h"
#include "core/arena.h"
#include "core/error.h"
#include "core/json.h"
#include "core/slice.h"
#include "core/string.h"

/* Subsystems */
#include "daemon.h"
#include "migration.h"
#include "onboard.h"
#ifdef HU_HAS_SKILLS
#include "skillforge.h"
#endif

/* Vtable interfaces (Phase 2) */
#include "channel.h"
#include "hardware.h"
#include "memory.h"
#include "observer.h"
#include "peripheral.h"
#include "provider.h"
#include "runtime.h"
#include "tool.h"

#include "agent/mailbox.h"
#include "agent/profile.h"
#include "agent/spawn.h"
#include "channels/thread_binding.h"
#ifdef HU_HAS_OTEL
#include "observability/otel.h"
#endif
#include "plugin.h"
#include "security/policy_engine.h"
#include "security/replay.h"

#endif /* HU_HUMAN_H */
