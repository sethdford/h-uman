#ifndef HU_LSP_H
#define HU_LSP_H
#include "human/tool.h"
typedef struct hu_lsp_diagnostic {
    int line;
    int col;
    int severity;
    char message[256];
} hu_lsp_diagnostic_t;
typedef struct hu_lsp_completion {
    char label[128];
    char detail[256];
    char insert_text[256];
} hu_lsp_completion_t;
typedef struct hu_lsp_location {
    char uri[512];
    int line;
    int col;
} hu_lsp_location_t;
#endif
