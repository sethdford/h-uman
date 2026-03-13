#include "human/agent/orchestrator.h"
#include "human/agent/registry.h"
#include <stdio.h>
#include <string.h>

hu_error_t hu_orchestrator_create(hu_allocator_t *alloc, hu_orchestrator_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->next_task_id = 1;
    return HU_OK;
}

void hu_orchestrator_deinit(hu_orchestrator_t *orch) {
    (void)orch;
}

hu_error_t hu_orchestrator_register_agent(hu_orchestrator_t *orch, const char *agent_id,
                                         size_t id_len, const char *role, size_t role_len,
                                         const char *skills, size_t skills_len) {
    if (!orch || !agent_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (orch->agent_count >= HU_ORCHESTRATOR_MAX_AGENTS)
        return HU_ERR_SUBAGENT_TOO_MANY;

    hu_agent_capability_t *a = &orch->agents[orch->agent_count];
    memset(a, 0, sizeof(*a));

    size_t n = id_len < sizeof(a->agent_id) - 1 ? id_len : sizeof(a->agent_id) - 1;
    strncpy(a->agent_id, agent_id, n);
    a->agent_id[n] = '\0';
    a->agent_id_len = n;

    if (role && role_len > 0) {
        n = role_len < sizeof(a->role) - 1 ? role_len : sizeof(a->role) - 1;
        strncpy(a->role, role, n);
        a->role[n] = '\0';
        a->role_len = n;
    }

    if (skills && skills_len > 0) {
        n = skills_len < sizeof(a->skills) - 1 ? skills_len : sizeof(a->skills) - 1;
        strncpy(a->skills, skills, n);
        a->skills[n] = '\0';
        a->skills_len = n;
    }

    a->capacity = 1.0;
    orch->agent_count++;
    return HU_OK;
}

hu_error_t hu_orchestrator_propose_split(hu_orchestrator_t *orch, const char *goal,
                                         size_t goal_len, const char **subtasks,
                                         const size_t *subtask_lens, size_t subtask_count) {
    (void)goal;
    (void)goal_len;
    if (!orch || !subtasks)
        return HU_ERR_INVALID_ARGUMENT;
    if (orch->task_count + subtask_count > HU_ORCHESTRATOR_MAX_TASKS)
        return HU_ERR_SUBAGENT_TOO_MANY;

    for (size_t i = 0; i < subtask_count; i++) {
        hu_orchestrator_task_t *t = &orch->tasks[orch->task_count];
        memset(t, 0, sizeof(*t));
        t->id = orch->next_task_id++;
        t->status = HU_TASK_UNASSIGNED;
        t->depends_on = 0;
        t->priority = 1.0;

        const char *desc = subtasks[i];
        size_t desc_len = subtask_lens ? subtask_lens[i] : (desc ? strlen(desc) : 0);
        if (desc && desc_len > 0) {
            size_t n = desc_len < sizeof(t->description) - 1 ? desc_len : sizeof(t->description) - 1;
            strncpy(t->description, desc, n);
            t->description[n] = '\0';
            t->description_len = n;
        }
        orch->task_count++;
    }
    return HU_OK;
}

hu_error_t hu_orchestrator_assign_task(hu_orchestrator_t *orch, uint32_t task_id,
                                       const char *agent_id, size_t agent_id_len) {
    if (!orch || !agent_id)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < orch->task_count; i++) {
        hu_orchestrator_task_t *t = &orch->tasks[i];
        if (t->id == task_id) {
            size_t n =
                agent_id_len < sizeof(t->assigned_agent) - 1 ? agent_id_len : sizeof(t->assigned_agent) - 1;
            strncpy(t->assigned_agent, agent_id, n);
            t->assigned_agent[n] = '\0';
            t->assigned_agent_len = n;
            t->status = HU_TASK_ASSIGNED;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_orchestrator_complete_task(hu_orchestrator_t *orch, uint32_t task_id,
                                         const char *result, size_t result_len) {
    if (!orch)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < orch->task_count; i++) {
        hu_orchestrator_task_t *t = &orch->tasks[i];
        if (t->id == task_id) {
            size_t n = result_len < sizeof(t->result) - 1 ? result_len : sizeof(t->result) - 1;
            if (result && result_len > 0) {
                strncpy(t->result, result, n);
                t->result[n] = '\0';
            } else {
                t->result[0] = '\0';
            }
            t->result_len = n;
            t->status = HU_TASK_COMPLETED;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_orchestrator_fail_task(hu_orchestrator_t *orch, uint32_t task_id,
                                     const char *reason, size_t reason_len) {
    if (!orch)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < orch->task_count; i++) {
        hu_orchestrator_task_t *t = &orch->tasks[i];
        if (t->id == task_id) {
            size_t n = reason_len < sizeof(t->result) - 1 ? reason_len : sizeof(t->result) - 1;
            if (reason && reason_len > 0) {
                strncpy(t->result, reason, n);
                t->result[n] = '\0';
            } else {
                t->result[0] = '\0';
            }
            t->result_len = n;
            t->status = HU_TASK_FAILED;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_orchestrator_merge_results(hu_orchestrator_t *orch, hu_allocator_t *alloc,
                                         char **out, size_t *out_len) {
    if (!orch || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    size_t total = 0;
    for (size_t i = 0; i < orch->task_count; i++) {
        if (orch->tasks[i].status == HU_TASK_COMPLETED)
            total += 32 + orch->tasks[i].result_len;
    }
    if (total == 0)
        return HU_OK;

    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    for (size_t i = 0; i < orch->task_count; i++) {
        const hu_orchestrator_task_t *t = &orch->tasks[i];
        if (t->status != HU_TASK_COMPLETED)
            continue;
        int n = snprintf(buf + pos, total + 1 - pos, "Task %u: %.*s\n",
                         (unsigned)t->id, (int)t->result_len, t->result);
        if (n > 0 && pos + (size_t)n <= total)
            pos += (size_t)n;
    }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

bool hu_orchestrator_all_complete(const hu_orchestrator_t *orch) {
    if (!orch || orch->task_count == 0)
        return true;
    for (size_t i = 0; i < orch->task_count; i++) {
        if (orch->tasks[i].status != HU_TASK_COMPLETED)
            return false;
    }
    return true;
}

size_t hu_orchestrator_count_by_status(const hu_orchestrator_t *orch, hu_task_status_t status) {
    if (!orch)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < orch->task_count; i++) {
        if (orch->tasks[i].status == status)
            count++;
    }
    return count;
}

static bool task_dependency_satisfied(const hu_orchestrator_t *orch, uint32_t depends_on) {
    if (depends_on == 0)
        return true;
    for (size_t i = 0; i < orch->task_count; i++) {
        if (orch->tasks[i].id == depends_on)
            return orch->tasks[i].status == HU_TASK_COMPLETED;
    }
    return true;
}

hu_error_t hu_orchestrator_next_task(hu_orchestrator_t *orch, hu_orchestrator_task_t **out) {
    if (!orch || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    for (size_t i = 0; i < orch->task_count; i++) {
        hu_orchestrator_task_t *t = &orch->tasks[i];
        if (t->status == HU_TASK_UNASSIGNED && task_dependency_satisfied(orch, t->depends_on)) {
            *out = t;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

const char *hu_task_status_str(hu_task_status_t status) {
    switch (status) {
    case HU_TASK_UNASSIGNED:
        return "unassigned";
    case HU_TASK_ASSIGNED:
        return "assigned";
    case HU_TASK_IN_PROGRESS:
        return "in_progress";
    case HU_TASK_COMPLETED:
        return "completed";
    case HU_TASK_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

hu_error_t hu_orchestrator_load_from_registry(hu_orchestrator_t *orch,
                                               const hu_agent_registry_t *registry) {
    if (!orch || !registry)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < registry->count; i++) {
        const hu_named_agent_config_t *cfg = &registry->agents[i];
        if (!cfg->name)
            continue;

        const char *role = cfg->role ? cfg->role : "general";
        const char *caps = cfg->capabilities ? cfg->capabilities : "";

        hu_error_t err = hu_orchestrator_register_agent(
            orch, cfg->name, strlen(cfg->name),
            role, strlen(role), caps, strlen(caps));
        if (err == HU_ERR_SUBAGENT_TOO_MANY)
            break;
        if (err != HU_OK && err != HU_ERR_SUBAGENT_TOO_MANY)
            return err;
    }
    return HU_OK;
}
