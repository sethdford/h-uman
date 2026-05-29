/* src/doctor/check_local_voice.c
 *
 * AC-3 doctor check — see header for the verdict contract. Like the sibling
 * check_reaction_collection_wired, the HU_ENABLE_CURL state is read from a
 * compile-time #ifdef of THIS translation unit, so the verdict reflects the
 * actual flags the running binary was built with.
 */

#include "human/doctor/check_local_voice.h"

#include "human/config.h"
#include "human/config_types.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef HU_ENABLE_CURL
#define HU_BUILT_WITH_CURL 1
#else
#define HU_BUILT_WITH_CURL 0
#endif

static char s_reason_buf[512];
static char s_detail_json_buf[512];

static bool file_present_nonempty(const char *path) {
    if (!path || path[0] == '\0')
        return false;
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return st.st_size > 0;
}

static const char *routing_name(int routing) {
    if (routing == HU_MLX_LOCAL_ROUTING_OFF)
        return "OFF";
    if (routing == HU_MLX_LOCAL_ROUTING_FORCE)
        return "FORCE";
    return "AUTO";
}

/* Pure verdict logic shared by the production runner and the test seam. */
static hu_doctor_check_result_t derive(int routing, bool model_configured, bool url_configured,
                                       bool adapter_present, bool curl_built, char *rb, size_t rcap,
                                       char *db, size_t dcap) {
    const char *rname = routing_name(routing);
    snprintf(db, dcap,
             "{\"routing\":\"%s\",\"model_configured\":%s,\"url_configured\":%s,"
             "\"adapter_present\":%s,\"curl_built\":%s}",
             rname, model_configured ? "true" : "false", url_configured ? "true" : "false",
             adapter_present ? "true" : "false", curl_built ? "true" : "false");

    if (routing == HU_MLX_LOCAL_ROUTING_OFF)
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA, "local-voice routing OFF — replies use the cloud model", db};

    if (!model_configured && !url_configured)
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA,
            "local-voice not configured (no mlx_local model/url) — running cloud-only", db};

    if (!curl_built) {
        snprintf(rb, rcap,
                 "local-voice intended (routing=%s) but binary built WITHOUT HU_ENABLE_CURL — "
                 "MLX serving and nightly adapter swap are compiled out. Rebuild with the release "
                 "preset (HU_ENABLE_CURL=ON) and restart the daemon.",
                 rname);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, rb, db};
    }

    if (!url_configured)
        return (hu_doctor_check_result_t){
            HU_DOCTOR_FAIL,
            "local-voice intended but mlx_local provider base_url is not configured — every turn "
            "falls back to cloud. Add an mlx_local provider entry with base_url.",
            db};

    if (!adapter_present)
        return (hu_doctor_check_result_t){
            HU_DOCTOR_FAIL,
            "local-voice intended but the LoRA adapter file is missing or empty at "
            "personalization.lora_adapter_path — set the path or train an adapter.",
            db};

    snprintf(rb, rcap,
             "local-voice ready (routing=%s): adapter present, mlx_local url configured, curl "
             "built. AUTO turns route local when the runtime health probe agrees.",
             rname);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, rb, db};
}

static hu_doctor_check_result_t check_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    const hu_doctor_check_local_voice_ctx_t *pctx = (const hu_doctor_check_local_voice_ctx_t *)ctx;
    const struct hu_config *cfg = pctx ? pctx->cfg : NULL;
    if (!cfg)
        return (hu_doctor_check_result_t){
            HU_DOCTOR_NA, "no config provided to doctor — local_voice check skipped", NULL};

    int routing = cfg->agent.mlx_local_routing;
    bool model_configured =
        cfg->agent.mr_mlx_local_model != NULL && cfg->agent.mr_mlx_local_model[0] != '\0';
    const char *url = hu_config_get_provider_base_url(cfg, "mlx_local");
    bool url_configured = url != NULL && url[0] != '\0';
    bool adapter_present = file_present_nonempty(cfg->personalization.lora_adapter_path);

    return derive(routing, model_configured, url_configured, adapter_present,
                  HU_BUILT_WITH_CURL != 0, s_reason_buf, sizeof(s_reason_buf), s_detail_json_buf,
                  sizeof(s_detail_json_buf));
}

hu_doctor_check_result_t
hu_doctor_check_local_voice_run_for_test(int routing, bool model_configured, bool url_configured,
                                         bool adapter_present, bool curl_built) {
    static char t_reason[512];
    static char t_detail[512];
    return derive(routing, model_configured, url_configured, adapter_present, curl_built, t_reason,
                  sizeof(t_reason), t_detail, sizeof(t_detail));
}

hu_doctor_check_t hu_doctor_check_local_voice = {
    .name = "local_voice",
    .description = "Reports local Gemma+LoRA voice-path readiness (routing, adapter, url, curl)",
    .run = check_run,
    .fix = NULL,
    .user_data = NULL,
};
