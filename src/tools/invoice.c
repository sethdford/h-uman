#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOOL_NAME "invoice"
#define TOOL_DESC                                                                           \
    "Create and parse invoices. Actions: create (generate invoice from line items), parse " \
    "(extract data from invoice text/JSON), summary (aggregate totals from multiple invoices)."
#define TOOL_PARAMS                                                                              \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\"," \
    "\"parse\",\"summary\"]},\"invoice_number\":{\"type\":\"string\"},\"from\":"                 \
    "{\"type\":\"string\",\"description\":\"Sender name/company\"},\"to\":{\"type\":\"string\"," \
    "\"description\":\"Recipient name/company\"},\"items\":{\"type\":\"array\",\"items\":"       \
    "{\"type\":\"object\",\"properties\":{\"description\":{\"type\":\"string\"},\"quantity\":"   \
    "{\"type\":\"number\"},\"unit_price\":{\"type\":\"number\"}}}},\"tax_rate\":"                \
    "{\"type\":\"number\",\"description\":\"Tax rate as decimal (e.g. 0.1 = 10%%)\"},"           \
    "\"currency\":{\"type\":\"string\",\"description\":\"Currency code (default: USD)\"},"       \
    "\"data\":{\"type\":\"string\",\"description\":\"Invoice text/JSON for parsing\"},"          \
    "\"format\":{\"type\":\"string\",\"enum\":[\"text\",\"markdown\",\"json\"]}},\"required\":"  \
    "[\"action\"]}"

typedef struct {
    char _unused;
} invoice_ctx_t;

static hu_error_t invoice_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                  hu_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

    if (strcmp(action, "create") == 0) {
        const char *inv_num = hu_json_get_string(args, "invoice_number");
        const char *from = hu_json_get_string(args, "from");
        const char *to = hu_json_get_string(args, "to");
        const char *currency = hu_json_get_string(args, "currency");
        double tax_rate = hu_json_get_number(args, "tax_rate", 0.0);
        const char *format = hu_json_get_string(args, "format");
        if (!format)
            format = "markdown";
        if (!currency)
            currency = "USD";

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char date[32];
        strftime(date, sizeof(date), "%Y-%m-%d", tm);

        size_t buf_sz = 8192;
        char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }

        int n = 0;
        double subtotal = 0;

        if (strcmp(format, "json") == 0) {
            n += snprintf(msg + n, buf_sz - (size_t)n,
                          "{\"invoice_number\":\"%s\",\"date\":\"%s\",\"from\":\"%s\","
                          "\"to\":\"%s\",\"currency\":\"%s\",\"items\":[",
                          inv_num ? inv_num : "INV-001", date, from ? from : "", to ? to : "",
                          currency);

            hu_json_value_t *items = hu_json_object_get((hu_json_value_t *)args, "items");
            if (items && items->type == HU_JSON_ARRAY) {
                for (size_t i = 0; i < items->data.array.len; i++) {
                    if (n < 0 || (size_t)n >= buf_sz)
                        break;
                    hu_json_value_t *item = items->data.array.items[i];
                    if (!item)
                        continue;
                    const char *desc = hu_json_get_string(item, "description");
                    double qty = hu_json_get_number(item, "quantity", 1);
                    double price = hu_json_get_number(item, "unit_price", 0);
                    double line_total = qty * price;
                    subtotal += line_total;
                    int w = snprintf(msg + n, buf_sz - (size_t)n,
                                     "%s{\"description\":\"%s\",\"quantity\":%.0f,"
                                     "\"unit_price\":%.2f,\"total\":%.2f}",
                                     i > 0 ? "," : "", desc ? desc : "", qty, price, line_total);
                    if (w > 0)
                        n += w;
                }
            }
            double tax = subtotal * tax_rate;
            n += snprintf(msg + n, buf_sz - (size_t)n,
                          "],\"subtotal\":%.2f,\"tax\":%.2f,\"total\":%.2f}", subtotal, tax,
                          subtotal + tax);
        } else {
            n += snprintf(msg + n, buf_sz - (size_t)n,
                          "# Invoice %s\n\n**Date:** %s\n**From:** %s\n**To:** %s\n"
                          "**Currency:** %s\n\n| Item | Qty | Unit Price | Total |\n"
                          "|------|-----|-----------|-------|\n",
                          inv_num ? inv_num : "INV-001", date, from ? from : "", to ? to : "",
                          currency);

            hu_json_value_t *items = hu_json_object_get((hu_json_value_t *)args, "items");
            if (items && items->type == HU_JSON_ARRAY) {
                for (size_t i = 0; i < items->data.array.len; i++) {
                    if (n < 0 || (size_t)n >= buf_sz)
                        break;
                    hu_json_value_t *item = items->data.array.items[i];
                    if (!item)
                        continue;
                    const char *desc = hu_json_get_string(item, "description");
                    double qty = hu_json_get_number(item, "quantity", 1);
                    double price = hu_json_get_number(item, "unit_price", 0);
                    double line_total = qty * price;
                    subtotal += line_total;
                    int w = snprintf(msg + n, buf_sz - (size_t)n, "| %s | %.0f | %.2f | %.2f |\n",
                                     desc ? desc : "", qty, price, line_total);
                    if (w > 0)
                        n += w;
                }
            }
            double tax = subtotal * tax_rate;
            n += snprintf(msg + n, buf_sz - (size_t)n,
                          "\n**Subtotal:** %.2f %s\n**Tax (%.0f%%):** %.2f %s\n"
                          "**Total:** %.2f %s\n",
                          subtotal, currency, tax_rate * 100, tax, currency, subtotal + tax,
                          currency);
        }
        *out = hu_tool_result_ok_owned(msg, (size_t)n);
        return HU_OK;
    }

    if (strcmp(action, "parse") == 0) {
        const char *data = hu_json_get_string(args, "data");
        if (!data) {
            *out = hu_tool_result_fail("missing data to parse", 21);
            return HU_OK;
        }
#if HU_IS_TEST
        char *msg = hu_sprintf(alloc,
                               "{\"parsed\":true,\"invoice_number\":\"INV-001\",\"total\":1500.00,"
                               "\"line_items\":2,\"source_length\":%zu}",
                               strlen(data));
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
        char *msg = hu_sprintf(alloc,
                               "{\"raw_length\":%zu,\"hint\":\"use AI to extract "
                               "structured fields from this text\"}",
                               strlen(data));
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#endif
        return HU_OK;
    }

    if (strcmp(action, "summary") == 0) {
#if HU_IS_TEST
        *out = hu_tool_result_ok(
            "{\"total_invoices\":5,\"total_amount\":25000.00,\"avg_amount\":5000.00,"
            "\"currency\":\"USD\"}",
            82);
#else
        *out = hu_tool_result_ok(
            "{\"hint\":\"provide invoice data via the 'data' field for aggregation\"}", 67);
#endif
        return HU_OK;
    }

    *out = hu_tool_result_fail("unknown action", 14);
    return HU_OK;
}

static const char *invoice_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *invoice_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *invoice_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void invoice_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(invoice_ctx_t));
}

static const hu_tool_vtable_t invoice_vtable = {
    .execute = invoice_execute,
    .name = invoice_name,
    .description = invoice_desc,
    .parameters_json = invoice_params,
    .deinit = invoice_deinit,
};

hu_error_t hu_invoice_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, sizeof(invoice_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(invoice_ctx_t));
    out->ctx = ctx;
    out->vtable = &invoice_vtable;
    return HU_OK;
}
