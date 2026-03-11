#ifndef HU_PERSONA_TRAINING_H
#define HU_PERSONA_TRAINING_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- F163: Style Drift Detection --- */

typedef struct hu_style_snapshot {
    double avg_msg_length;
    double emoji_rate;
    double abbreviation_rate;
    double question_rate;
    double capitalization_rate;
    uint64_t timestamp;
} hu_style_snapshot_t;

double hu_style_drift_score(const hu_style_snapshot_t *baseline,
                            const hu_style_snapshot_t *current);
bool hu_style_drift_significant(double drift_score, double threshold);
hu_error_t hu_style_drift_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_style_drift_insert_sql(const hu_style_snapshot_t *snap, char *buf, size_t cap,
                                     size_t *out_len);

/* --- F165-F166: Behavioral Cloning Calibration --- */

typedef struct hu_clone_calibration {
    char *parameter;
    size_t parameter_len; /* "formality", "emoji_freq", "msg_length" */
    double target_value;  /* from real message analysis */
    double current_value; /* current persona setting */
    double delta;         /* target - current */
} hu_clone_calibration_t;

hu_error_t hu_clone_calibrate(hu_allocator_t *alloc, const char **param_names,
                              const size_t *param_lens, const double *targets,
                              const double *currents, size_t param_count,
                              hu_clone_calibration_t *out);
hu_error_t hu_clone_build_adjustment_prompt(hu_allocator_t *alloc,
                                            const hu_clone_calibration_t *cals, size_t count,
                                            char **out, size_t *out_len);
void hu_clone_calibration_deinit(hu_allocator_t *alloc, hu_clone_calibration_t *cal);

/* --- F144-F146: Persona LoRA --- */

typedef enum hu_lora_status {
    HU_LORA_NOT_STARTED = 0,
    HU_LORA_COLLECTING,
    HU_LORA_TRAINING,
    HU_LORA_READY,
    HU_LORA_ACTIVE
} hu_lora_status_t;

typedef struct hu_lora_config {
    uint32_t min_training_messages; /* default 500 */
    uint32_t epochs;                /* default 3 */
    double learning_rate;           /* default 2e-4 */
    uint32_t rank;                 /* LoRA rank, default 16 */
    char *base_model;
    size_t base_model_len;
} hu_lora_config_t;

typedef struct hu_lora_state {
    hu_lora_status_t status;
    uint32_t messages_collected;
    uint32_t messages_needed;
    char *model_path;
    size_t model_path_len; /* path to trained adapter */
} hu_lora_state_t;

hu_lora_config_t hu_lora_default_config(void);
hu_lora_status_t hu_lora_check_readiness(uint32_t messages_collected,
                                        uint32_t min_required);
hu_error_t hu_lora_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_lora_insert_training_sample_sql(const char *message, size_t msg_len,
                                               const char *context, size_t ctx_len,
                                               uint64_t timestamp, char *buf, size_t cap,
                                               size_t *out_len);
hu_error_t hu_lora_query_status_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_lora_build_prompt(hu_allocator_t *alloc, const hu_lora_state_t *state,
                                char **out, size_t *out_len);

const char *hu_lora_status_str(hu_lora_status_t s);
void hu_lora_state_deinit(hu_allocator_t *alloc, hu_lora_state_t *s);
void hu_lora_config_deinit(hu_allocator_t *alloc, hu_lora_config_t *c);

#endif /* HU_PERSONA_TRAINING_H */
