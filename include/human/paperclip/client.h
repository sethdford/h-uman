#ifndef HU_PAPERCLIP_CLIENT_H
#define HU_PAPERCLIP_CLIENT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

typedef struct hu_paperclip_client {
    hu_allocator_t *alloc;
    char *api_url;
    char *agent_id;
    char *company_id;
    char *api_key;
    char *run_id;
    char *task_id;
    char *wake_reason;
} hu_paperclip_client_t;

typedef struct hu_paperclip_task {
    char *id;
    char *title;
    char *description;
    char *status;
    char *priority;
    char *project_name;
    char *goal_title;
} hu_paperclip_task_t;

typedef struct hu_paperclip_task_list {
    hu_paperclip_task_t *tasks;
    size_t count;
} hu_paperclip_task_list_t;

typedef struct hu_paperclip_comment {
    char *id;
    char *body;
    char *author_name;
    char *created_at;
} hu_paperclip_comment_t;

typedef struct hu_paperclip_comment_list {
    hu_paperclip_comment_t *comments;
    size_t count;
} hu_paperclip_comment_list_t;

hu_error_t hu_paperclip_client_init(hu_paperclip_client_t *client, hu_allocator_t *alloc);
hu_error_t hu_paperclip_client_init_from_config(hu_paperclip_client_t *client,
                                                 hu_allocator_t *alloc, const char *api_url,
                                                 const char *agent_id, const char *company_id);
void hu_paperclip_client_deinit(hu_paperclip_client_t *client);

hu_error_t hu_paperclip_list_tasks(hu_paperclip_client_t *client,
                                    hu_paperclip_task_list_t *out);
hu_error_t hu_paperclip_get_task(hu_paperclip_client_t *client, const char *task_id,
                                  hu_paperclip_task_t *out);
hu_error_t hu_paperclip_checkout_task(hu_paperclip_client_t *client, const char *task_id);
hu_error_t hu_paperclip_update_task(hu_paperclip_client_t *client, const char *task_id,
                                     const char *status);
hu_error_t hu_paperclip_post_comment(hu_paperclip_client_t *client, const char *task_id,
                                      const char *body, size_t body_len);
hu_error_t hu_paperclip_get_comments(hu_paperclip_client_t *client, const char *task_id,
                                      hu_paperclip_comment_list_t *out);

void hu_paperclip_task_free(hu_allocator_t *alloc, hu_paperclip_task_t *task);
void hu_paperclip_task_list_free(hu_allocator_t *alloc, hu_paperclip_task_list_t *list);
void hu_paperclip_comment_list_free(hu_allocator_t *alloc, hu_paperclip_comment_list_t *list);

#endif /* HU_PAPERCLIP_CLIENT_H */
