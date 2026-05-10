#ifndef HU_INTELLIGENCE_WORLD_MODEL_H
#define HU_INTELLIGENCE_WORLD_MODEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/*
 * World Model — forward simulation and counterfactual reasoning
 * on top of the causal knowledge graph.
 *
 * Given an action and context, predicts likely outcomes by traversing
 * causal links, weighting by confidence, and ranking alternatives.
 */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

typedef struct hu_wm_prediction {
    char outcome[1024];
    size_t outcome_len;
    double confidence;   /* 0.0–1.0 */
    char reasoning[2048];
    size_t reasoning_len;
} hu_wm_prediction_t;

typedef struct hu_action_option {
    char action[512];
    size_t action_len;
    hu_wm_prediction_t prediction;
    double score; /* composite ranking score */
} hu_action_option_t;

typedef struct hu_causal_world_model {
    hu_allocator_t *alloc;
    sqlite3 *db;
} hu_causal_world_model_t;

hu_error_t hu_causal_world_model_create(hu_allocator_t *alloc, sqlite3 *db,
                                 hu_causal_world_model_t *out);
void hu_causal_world_model_deinit(hu_causal_world_model_t *model);

hu_error_t hu_causal_world_model_init_tables(hu_causal_world_model_t *model);

/* Predict outcome of an action given context. */
hu_error_t hu_world_simulate(hu_causal_world_model_t *model,
                             const char *action, size_t action_len,
                             const char *context, size_t context_len,
                             hu_wm_prediction_t *out);

/* "What if" — predict outcome of an alternative action. */
hu_error_t hu_world_counterfactual(hu_causal_world_model_t *model,
                                   const char *original_action, size_t orig_len,
                                   const char *alternative, size_t alt_len,
                                   const char *context, size_t ctx_len,
                                   hu_wm_prediction_t *out);

/* Rank multiple action options by predicted outcome quality. */
hu_error_t hu_world_evaluate_options(hu_causal_world_model_t *model,
                                     const char **actions, const size_t *action_lens,
                                     size_t count,
                                     const char *context, size_t ctx_len,
                                     hu_action_option_t *out);

/* Record an observed outcome to strengthen/weaken causal links. */
hu_error_t hu_world_record_outcome(hu_causal_world_model_t *model,
                                   const char *action, size_t action_len,
                                   const char *outcome, size_t outcome_len,
                                   double confidence, int64_t now_ts);

/* --- Context-Aware Simulation (AGI-W2, AGI-W5) --- */

typedef struct hu_world_context {
    char entities[8][128];  /* up to 8 relevant entities */
    size_t entity_count;
    int64_t time_window_start;
    int64_t time_window_end;
    char user_state[256];
    size_t user_state_len;
} hu_world_context_t;

hu_error_t hu_world_context_from_text(const char *text, size_t text_len,
                                       hu_world_context_t *out);

hu_error_t hu_world_simulate_with_context(hu_causal_world_model_t *model,
                                           const char *action, size_t action_len,
                                           const hu_world_context_t *ctx,
                                           hu_wm_prediction_t *out);

hu_error_t hu_world_compare_actions(hu_causal_world_model_t *model,
                                     const char **actions, const size_t *lens,
                                     size_t count, const hu_world_context_t *ctx,
                                     hu_action_option_t *ranked_out);

hu_error_t hu_world_what_if(hu_causal_world_model_t *model,
                             const char *action, size_t action_len,
                             const hu_world_context_t *ctx,
                             hu_wm_prediction_t *scenarios, size_t max_scenarios,
                             size_t *out_count);

/* Get causal chain depth from an action (how many hops of consequences). */
hu_error_t hu_world_causal_depth(hu_causal_world_model_t *model,
                                 const char *action, size_t action_len,
                                 size_t *out_depth);

/* --- Causal Graph Engine (AGI-W1) --- */

typedef enum {
    HU_EDGE_CAUSES = 0,
    HU_EDGE_PREVENTS,
    HU_EDGE_ENABLES,
    HU_EDGE_CORRELATES,
    HU_EDGE_TEMPORAL_FOLLOWS
} hu_causal_edge_type_t;

typedef struct hu_causal_node {
    int64_t id;
    char label[256];
    size_t label_len;
    char type[64];       /* "entity", "action", "outcome", "event" */
    size_t type_len;
    int64_t created_at;
} hu_causal_node_t;

typedef struct hu_causal_edge {
    int64_t id;
    int64_t source_id;
    int64_t target_id;
    hu_causal_edge_type_t type;
    double confidence;
    int evidence_count;
    int64_t last_observed;
} hu_causal_edge_t;

hu_error_t hu_world_add_node(hu_causal_world_model_t *model, const char *label, size_t label_len,
                             const char *type, size_t type_len, int64_t *out_id);

hu_error_t hu_world_add_edge(hu_causal_world_model_t *model, int64_t source, int64_t target,
                             hu_causal_edge_type_t type, double confidence, int64_t timestamp);

hu_error_t hu_world_get_neighbors(hu_causal_world_model_t *model, int64_t node_id,
                                  hu_causal_edge_t *edges, size_t max_edges, size_t *out_count);

hu_error_t hu_world_trace_causal_chain(hu_causal_world_model_t *model, int64_t start_node,
                                       int max_depth, hu_causal_node_t *path, size_t max_path,
                                       size_t *out_len);

hu_error_t hu_world_find_paths(hu_causal_world_model_t *model, int64_t from, int64_t to,
                               int max_depth, hu_causal_node_t *path, size_t max_path,
                               size_t *out_len);

hu_error_t hu_world_record_accuracy(hu_causal_world_model_t *model,
                                     const char *action, size_t action_len,
                                     const char *predicted, size_t predicted_len,
                                     const char *actual, size_t actual_len,
                                     double predicted_confidence);

hu_error_t hu_world_get_accuracy(hu_causal_world_model_t *model,
                                  double *accuracy_out, size_t *sample_count);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_INTELLIGENCE_WORLD_MODEL_H */
