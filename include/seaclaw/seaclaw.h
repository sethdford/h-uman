#ifndef SC_SEACLAW_H
#define SC_SEACLAW_H

/**
 * SeaClaw — autonomous AI assistant runtime (C11).
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
#include "skillforge.h"

/* Vtable interfaces (Phase 2) */
#include "channel.h"
#include "hardware.h"
#include "memory.h"
#include "observer.h"
#include "peripheral.h"
#include "provider.h"
#include "runtime.h"
#include "tool.h"


#include "agent/spawn.h"
#include "agent/mailbox.h"
#include "agent/profile.h"
#include "channels/thread_binding.h"
#include "security/policy_engine.h"
#include "security/replay.h"
#include "observability/otel.h"
#include "plugin.h"


#include "agent/spawn.h"
#include "agent/mailbox.h"
#include "agent/profile.h"
#include "channels/thread_binding.h"
#include "security/policy_engine.h"
#include "security/replay.h"
#include "observability/otel.h"
#include "plugin.h"

#endif /* SC_SEACLAW_H */
