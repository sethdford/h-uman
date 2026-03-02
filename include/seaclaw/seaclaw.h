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
#include "skillforge.h"
#include "onboard.h"
#include "daemon.h"
#include "migration.h"

/* Vtable interfaces (Phase 2) */
#include "provider.h"
#include "channel.h"
#include "tool.h"
#include "memory.h"
#include "observer.h"
#include "runtime.h"
#include "peripheral.h"
#include "hardware.h"

#endif /* SC_SEACLAW_H */
