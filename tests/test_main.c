#include "test_framework.h"

int hu__total = 0;
int hu__passed = 0;
int hu__failed = 0;
int hu__skipped = 0;
int hu__suite_active = 1;
const char *hu__suite_filter = NULL;
const char *hu__test_filter = NULL;
jmp_buf hu__jmp;

/* Flaky-test quarantine (see HU_RUN_TEST_FLAKY in test_framework.h).
 * Default 2 retries; overridable via HU_TEST_FLAKY_RETRIES (0 disables retry). */
int hu__flaky_retries = 2;
int hu__flaky_recovered = 0;
int hu__quiet_fail = 0;

void run_flaky_harness_tests(void);
void run_allocator_tests(void);
void run_data_loader_tests(void);
void run_agent_modules_tests(void);
void run_agent_definition_tests(void);
void run_agent_git_tests(void);
void run_agent_app_config_tests(void);
void run_task_store_tests(void);
void run_compaction_hierarchical_tests(void);
void run_tot_recursive_tests(void);
void run_agent_subsystems_tests(void);
void run_crypto_tests(void);
void run_json_tests(void);
void run_wasm_tests(void); /* from test_wasm.c when built */
void run_string_tests(void);
void run_string_ci_tests(void);
void run_rand_tests(void);
void run_log_once_tests(void);
void run_vertex_adc_tests(void);
void run_init_proposer_tests(void);
void run_init_proposer_compose_tests(void);
void run_init_outcome_tests(void);
void run_init_dpo_bridge_tests(void);
void run_prompt_budget_tests(void);
void run_prompt_budget_snapshot_tests(void);
void run_config_gated_subsystems_tests(void);
void run_silent_disable_compliance_tests(void);
void run_io_secure_tests(void);
void run_file_slurp_tests(void);
void run_config_banner_runtime_failfast_tests(void);
void run_slice_tests(void);
void run_tool_registry_honesty_tests(void);
void run_contextual_bandit_tests(void);
void run_humanization_bandit_tests(void);
void run_memory_tests(void);
void run_w7_render_null_safety_tests(void);
void run_mlx_load_adapter_tests(void);
void run_m3_route_per_turn_tests(void);
void run_m3_route_per_turn_call_sites_tests(void);
void run_m3_swap_failure_observability_tests(void);
void run_m3_outcome_ring_population_tests(void);
void run_m3_ab_fidelity_gate_tests(void);
void run_m3_frontier_auto_invocation_tests(void);
void run_sql_transaction_tests(void);
void run_sqlite_integrity_tests(void);
void run_memory_util_tests(void);
void run_tunnel_tests(void);
void run_gateway_tests(void);
void run_auth_tests(void);
void run_oauth_tests(void);
void run_security_tests(void);
void run_normalize_tests(void);
void run_sensitivity_tests(void);
void run_vault_tests(void);
void run_vault_aead_tests(void);
void run_app_bundle_structure_tests(void);
void run_pkg_builder_tests(void);
void run_install_docs_tests(void);
void run_sign_notarize_tests(void);
void run_provider_tests(void);
void run_provider_http_tests(void);
void run_gemini_vertex_auth_tests(void);
void run_ensemble_tests(void);
void run_api_key_tests(void);
void run_channel_tests(void);
void run_channel_class_tests(void);
void run_channel_format_tests(void);
void run_channel_behavior_class_tests(void);
void run_channel_rate_limit_tests(void);
void run_channel_http_tests(void);
void run_webhook_channel_tests(void);
void run_bg_registry_tests(void);
void run_channel_embeds_tests(void);
/* Phase 2 Task 10 (RL SOTA): hu_reaction_event_t + iMessage/Slack normalizers. */
void run_reaction_event_tests(void);
/* Phase 2 Task 11 (RL SOTA): hu_imessage_poll_reactions tapback inbound poll. */
void run_imessage_reactions_tests(void);
void run_imessage_caps_tests(void);
void run_imessage_bb_event_tests(void);
void run_imessage_emoji_reaction_tests(void);
/* Phase 1a of docs/plans/2026-05-18-imessage-sota.md: pure synthesis primitives
 * that render iMessage events into canonical English for personal-model ingest. */
void run_imessage_ingest_tests(void);
/* Phase 1c end-to-end smoke: chat.db → poll → reaction_handler → personal_model. */
void run_imessage_personal_model_e2e_tests(void);
/* Phase 3: bplist00 parser tests (pure-C, no platform gates). */
void run_bplist_tests(void);
/* Phase 3 completion: daemon iMessage observer tick. */
void run_imessage_observer_tests(void);
/* Phase 6: schema-version probe + drift canary. */
void run_imessage_schema_tests(void);
/* Phase 4: typedstream attribute-run parser tests. */
void run_typedstream_tests(void);
/* M3 B4 T4: streaming-safe harmony channel-marker filter. */
void run_harmony_filter_tests(void);
/* Phase 5: per-balloon payload decoder tests + privacy contracts. */
void run_imessage_balloon_decode_tests(void);
/* Cross-channel reaction emit: WhatsApp + Matrix. */
void run_whatsapp_reactions_tests(void);
void run_matrix_reactions_tests(void);
/* SQLite-backed reaction_handler lookup store. */
void run_reaction_handler_lookup_store_tests(void);
/* Conversation-gap classifier (Tier 1 #3 of better-than-human plan). */
void run_imessage_gaps_tests(void);
/* Pattern-drift compute layer (Tier-3 future-facing). */
void run_pattern_drift_tests(void);
/* Per-contact relationship signatures (Tier 2 Sprint A). */
void run_contact_signature_tests(void);
/* Sprint A.5 — render reaction signature to prompt paragraph. */
void run_persona_social_insights_tests(void);
/* Sprint A.6 — daemon social tick (gap + drift + signatures → JSON). */
void run_daemon_social_tick_tests(void);
/* Reaction-driven topic salience (Tier 1 #1). */
void run_personal_model_topics_from_reactions_tests(void);
/* Calibrate-with-reactions (Tier 1 #2). */
void run_calibration_reactions_tests(void);
/* Predictive draft suggestions (Sprint 1 Story 1). */
void run_predictive_drafts_tests(void);
/* Phase 2 cross-channel reaction emit: Discord MESSAGE_REACTION_ADD/REMOVE. */
void run_discord_reactions_tests(void);
/* Phase 2 cross-channel reaction emit: Telegram message_reaction diff. */
void run_telegram_reactions_tests(void);
/* Phase 2 Task 12 (RL SOTA): Slack reaction_added/removed webhook branch. */
void run_slack_reactions_tests(void);
/* Phase 2 Task 13 (RL SOTA): reaction_handler event → dpo_pairs row E2E.
 * Test TU (tests/test_reaction_handler_e2e.c) calls sqlite3_* directly to
 * seed the dpo_pairs collector, so the test source is gated by
 * HU_ENABLE_SQLITE in CMakeLists.txt. Mirror that gate here so the
 * forward decl + call site don't reference a missing symbol in
 * minimal-build / no-sqlite / cross-arm64 variants. */
#ifdef HU_ENABLE_SQLITE
void run_reaction_handler_e2e_tests(void);
#endif
void run_declarative_tools_tests(void);
void run_skill_trust_tests(void);
void run_tool_tests(void);
void run_webhook_tests(void);
void run_hook_pipeline_tests(void);
void run_agent_dispatch_hooks_tests(void);
void run_compaction_structured_tests(void);
void run_instruction_discover_tests(void);
void test_vtables_run(void);
void run_peripheral_tests(void);
void run_e2e_tests(void);
void run_e2e_conversation_tests(void);
void run_e2e_agent_loop_tests(void);
void run_subsystems_tests(void);
void run_onboard_state_tests(void);
void run_onboard_dispatcher_tests(void);
void run_onboard_step1_tests(void);
void run_onboard_nextstep_tests(void);
void run_onboard_aloop_tests(void);
void run_config_parse_tests(void);
void run_config_migrate_tests(void);
void run_adversarial_tests(void);
void run_adversarial_detect_tests(void);
void run_gateway_http_tests(void);
void run_memory_full_tests(void);
void run_tools_all_tests(void);
void run_rag_tests(void);
void run_multimodal_tests(void);
void run_multimodal_pipeline_tests(void);
void run_multimodal_memory_tests(void);
void run_multimodal_audio_tests(void);
void run_multimodal_video_tests(void);
void run_voice_duplex_tests(void);
void run_turn_signal_tests(void);
void run_voice_rt_openai_tests(void);
void run_voice_provider_tests(void);
void run_voice_session_tests(void);
void run_gemini_live_tests(void);
void run_voice_factory_e2e_tests(void);
void run_voice_streaming_e2e_tests(void);
void run_mlx_local_voice_tests(void);
void run_autonomy_tests(void);
void run_retrieval_tests(void);
void run_retrieval_contact_isolation_tests(void);
void run_vector_tests(void);
void run_vector_full_tests(void);
void run_infrastructure_tests(void);
void run_memory_subsystems_tests(void);
void run_http_tests(void);
void run_sse_tests(void);
void run_streaming_tests(void);
void run_websocket_tests(void);
void run_ws_integration_tests(void);
void run_net_security_tests(void);
void run_path_security_tests(void);
void run_process_util_tests(void);
void run_prompt_tests(void);
void run_prompt_trim_tests(void);
void run_gate_mode_tests(void);
void run_graph_grounding_tests(void);
void run_uncertainty_tests(void);
void run_tool_search_tests(void);
void run_persona_tests(void);
void run_terseness_tests(void);
void run_circadian_tests(void);
void run_relationship_tests(void);
void run_replay_tests(void);
void run_style_clone_tests(void);
void run_uncertainty_tests(void);
void run_life_sim_tests(void);
void run_persona_mood_tests(void);
void run_persona_feedback_tests(void);
void run_persona_examples_style_tests(void);
void run_persona_filler_roundtrip_tests(void);
void run_persona_cli_tests(void);
void run_persona_sticker_tests(void);
void run_voice_maturity_tests(void);
void run_style_learner_tests(void);
void run_persona_refresh_tests(void);
void run_persona_rag_tests(void);
void run_temporal_tests(void);
void run_inner_world_tests(void);
void run_persona_eval_tests(void);
/* Moment Context Decision Layer (Task 0.2) — scaffolding; tests landed in Phase 1/2. */
void run_moment_compose_tests(void);
void run_moment_render_tests(void);
void run_behavior_policy_tests(void);
void run_behavior_dialog_act_tests(void);
void run_behavior_affect_tests(void);
void run_behavior_change_tests(void);
void run_behavior_safety_tests(void);
void run_behavior_prosocial_tests(void);
void run_win_detect_tests(void);
void run_prosocial_moment_tests(void);
void run_celebration_tests(void);
#ifdef HU_ENABLE_SQLITE
void run_celebration_repo_tests(void);
#endif
void run_behavior_prompt_tests(void);
void run_behavior_support_strategy_tests(void);
void run_behavior_trust_tests(void);
void run_tapback_band_tests(void);
void run_behavior_corpora_tests(void);
void run_user_sim_tests(void);
void run_tom_scenario_tests(void);
void run_behavior_trust_prompt_tests(void);
void run_behavior_pressure_tests(void);
void run_sycophancy_pack_tests(void);
void run_longmemeval_tests(void);
void run_user_sim_scenario_tests(void);
void run_chronotype_tests(void);
void run_lifecycle_tests(void);
void run_observer_tests(void);
void run_session_tests(void);
void run_bus_tests(void);
void run_identity_tests(void);
void run_channel_manager_tests(void);
void run_new_modules_tests(void);
void run_provider_all_tests(void);
void run_chat_response_diag_tests(void);
void run_channel_all_tests(void);
void run_idempotency_tests(void);
void run_idempotency_hula_integration_tests(void);
void run_meta_common_tests(void);
void run_channel_integration_tests(void);
void run_channel_vtable_action_surface_tests(void);
void run_config_extended_tests(void);
void run_config_getters_tests(void);
void run_config_validation_tests(void);
void run_config_action_surface_tests(void);
void run_config_seth_voice_defaults_tests(void);
void run_json_extended_tests(void);
void run_security_extended_tests(void);
void run_security_pipeline_tests(void);
void run_core_extended_tests(void);
void run_gateway_extended_tests(void);
void run_gateway_auth_tests(void);
void run_gateway_voice_tests(void);
void run_gateway_hula_traces_tests(void);
void run_pairing_tests(void);
void run_agent_extended_tests(void);
void run_agent_security_tests(void);
void run_agent_teams_tests(void);
void run_delegation_tests(void);
void run_diagnostic_commands_tests(void);
void run_skills_tests(void);
void run_memory_new_tests(void);
void run_ported_modules_tests(void);
void run_doctor_imessage_diagnose_tests(void);
void run_doctor_registry_tests(void);
void run_doctor_chatdb_tests(void);
void run_doctor_check_provider_tests(void);
void run_doctor_exit_codes_tests(void);
void run_doctor_json_output_tests(void);
void run_doctor_reaction_collection_wired_tests(void);
void run_doctor_prompt_budget_tests(void);
void run_outbound_sanitize_tests(void);
void run_daemon_follow_up_watcher_tests(void);
void run_cli_ctl_tests(void);
void run_doctor_local_voice_tests(void);
void run_onboard_step_provider_tests(void);
void run_cron_tests(void);
void run_cron_session_tools_tests(void);
void run_subagent_tests(void);
void run_task_manager_tests(void);
void run_task_tools_tests(void);
void run_tool_ask_user_tests(void);
void run_mcp_tests(void);
void run_mcp_jsonrpc_tests(void);
void run_mcp_manager_tests(void);
void run_mcp_resource_tools_tests(void);
void run_mcp_transport_tests(void);
void run_mcp_transport_sse_tests(void);
void run_mcp_http_integration_tests(void);
void run_otel_trace_tests(void);
void run_mcp_audit_tests(void);
void run_voice_tests(void);
void run_cli_tests(void);
void run_diagnose_notary_tests(void);
void run_update_tests(void);
void run_vector_stores_tests(void);
void run_memory_engines_ext_tests(void);
void run_memory_poisoning_tests(void);
void run_runtime_tests(void);
void run_runtime_bundle_tests(void);
void run_channel_loop_tests(void);
void run_util_modules_tests(void);
void run_roadmap_tests(void);
void run_new_features_tests(void);
void run_ollama_integration_tests(void);
void run_plugin_tests(void);
void run_tenant_tests(void);
void run_gmail_tests(void);
void run_imessage_extended_tests(void);
void run_imessage_reply_style_tests(void);
void run_imessage_reply_fallback_quote_tests(void);
void run_imessage_private_protocol_tests(void);
void run_imessage_private_client_tests(void);
void run_imessage_chatdb_fixture_tests(void);
void run_imessage_adversarial_tests(void);
void run_imessage_non_allowlisted_tests(void);
void run_imessage_rich_link_tests(void);
void run_imessage_react_contract_tests(void);
void run_imessage_reply_pacing_tests(void);
void run_imessage_action_telemetry_tests(void);
void run_follow_up_tests(void);
void run_imessage_action_telemetry_tests(void);
void run_imessage_reply_pacing_tests(void);
void run_imessage_threaded_reply_tests(void);
void run_imessage_custom_tapback_tests(void);
void run_imessage_action_facts_tests(void);
void run_imessage_dispatcher_tests(void);
void run_imessage_sticker_tests(void);
void run_follow_up_tests(void);
void run_follow_up_daemon_integration_tests(void);
void run_daemon_aloop_smoke_tests(void);
void run_intelligence_tests(void);
void run_protective_tests(void);
void run_humor_tests(void);
void run_authentic_tests(void);
void run_rag_pipeline_tests(void);
void run_persona_training_tests(void);
void run_behavioral_tests(void);
void run_context_ext_tests(void);
void run_untested_modules_tests(void);
void run_modules_coverage_tests(void);
void run_coverage_new_tests(void);
void run_context_tests(void);
void run_qmd_tests(void);
void run_terminal_tests(void);
void run_tavily_tests(void);
void run_awareness_tests(void);
void run_entropy_gate_tests(void);
void run_episodic_tests(void);
void run_reflection_tests(void);
void run_input_guard_tests(void);
void run_externalization_tests(void);
void run_conversation_tests(void);
void run_vision_tests(void);
void run_ab_response_tests(void);
void run_event_extract_tests(void);
void run_stm_tests(void);
void run_emotional_graph_tests(void);
void run_comfort_patterns_tests(void);
void run_emotional_moments_tests(void);
void run_emotional_state_tests(void);
void run_contact_style_overlay_tests(void);
void run_graph_tests(void);
void run_w1_bitemporal_tests(void);
void run_w2_autodream_tests(void);
void run_w3_multigraph_tests(void);
void run_w4_verifier_tests(void);
void run_w5_persona_deltas_tests(void);
void run_persona_delta_observer_tests(void);
void run_world_model_bridge_tests(void);
void run_signal_channel_wire_tests(void);
void run_daemon_housekeeping_tests(void);
void run_orphan_channel_audit_tests(void);
void run_verifier_metrics_tests(void);
void run_doctor_ws_consumer_tests(void);
void run_output_validator_tests(void);
void run_chain_failure_paths_tests(void);
void run_agent_fail_path_regressions_tests(void);
void run_stop_sequences_tests(void);
void run_validators_builtin_tests(void);
void run_pattern_c_paths_tests(void);
void run_validators_persona_safety_tests(void);
void run_validator_reject_discards_tests(void);
void run_validator_telemetry_tests(void);
void run_validator_chain_cache_tests(void);
void run_daemon_e2e_validator_tests(void);
void run_response_guard_tests(void);
void run_response_guard_retry_tests(void);
void run_outbound_pipeline_tests(void);
void run_outbound_strip_tests(void);
void run_style_governor_tests(void);
void run_outbound_shape_tests(void);
void run_outbound_echo_tests(void);
void run_outbound_crosstalk_tests(void);
/* Sprint 60 follow-up — SQLite-backed crosstalk lookup. Tests seed
 * the messages table directly via sqlite3, so source + tests are
 * gated by HU_ENABLE_SQLITE in CMakeLists.txt. Mirror that gate here. */
#ifdef HU_ENABLE_SQLITE
void run_boundary_repo_tests(void);
void run_opinions_repo_tests(void);
void run_life_chapter_repo_tests(void);
void run_social_graph_repo_tests(void);
void run_self_awareness_repo_tests(void);
void run_feed_items_repo_tests(void);
void run_memories_repo_tests(void);
void run_emotional_moments_repo_tests(void);
void run_emotional_residue_repo_tests(void);
void run_emotional_state_repo_tests(void);
void run_mood_repo_tests(void);
void run_self_model_repo_tests(void);
void run_theory_of_mind_repo_tests(void);
void run_outbound_crosstalk_sqlite_tests(void);
/* Sprint 60 E2E SOTA proof — full pipeline through a file-based db. */
void run_outbound_e2e_sota_proof_tests(void);
/* Sprint 60 — burst-egress helper (daemon-side burst-loop wiring). */
void run_burst_egress_tests(void);
/* Sprint 60 — E2E proof that pipeline.c stats wiring fires on real runs. */
void run_outbound_stats_e2e_tests(void);
/* Sprint 60 — pipeline P50/P95/P99 latency budget. */
void run_outbound_pipeline_perf_tests(void);
#endif
void run_outbound_persona_tests(void);
/* Sprint 60 — persona stage ML wiring (shape-classifier upgrade). */
void run_outbound_persona_classifier_tests(void);
void run_outbound_moderation_tests(void);
void run_outbound_corpus_regression_tests(void);
/* Sprint 60 — outbound stats (per-stage × per-verdict counters). */
void run_outbound_stats_tests(void);
/* Sprint 60 — doctor check exposing outbound stats. */
void run_doctor_outbound_stats_tests(void);
/* M3 dispatch — doctor check for G9 retry-outcome health. */
void run_doctor_unified_dispatch_tests(void);
void run_multimodal_policy_tests(void);
void run_persona_eval_tests(void);
void run_agent_tests(void);                        /* Sprint 46 R5.3 carryover */
void run_agent_turn_state_tests(void);             /* #26: per-turn state tracking */
void run_agent_turn_transport_tests(void);         /* M4 follow-up: transport-error fast-fail */
void run_agent_turn_request_overrides_tests(void); /* G11: per-turn override parity */
void run_w6_e2e_adversarial_tests(void);
void run_w7_memory_facade_tests(void);
void run_w8_belief_layer_tests(void);
void run_w9_world_model_tests(void);
void run_w10_neural_memory_tests(void);
void run_w11_self_rag_tests(void);
void run_w12_planner_tests(void);
void run_w12_verifier_loop_tests(void);
#ifdef HU_ENABLE_LEARNING
void run_w13_learner_tests(void);
void run_w14_runners_tests(void);
void run_w14_lora_retrain_tests(void);
void run_w14_dual_lora_tests(void);
void run_learner_bridge_tests(void);
#endif
void run_w15_backup_restore_tests(void);
void run_w14_scheduler_tests(void);
/* Spec 2026-05-19 — DPO pair-count auto-training trigger. */
void run_dpo_pair_count_trigger_tests(void);
void run_training_runner_shared_entry_tests(void);
/* US-8 / M3 frontier-MLX dispatch via training_loop.py subprocess. */
void run_m3_frontier_mlx_dispatch_tests(void);
/* Spec 2026-05-19 self-model-scaffold — Phase A. Test file uses the
 * internal-#ifdef-wrap-with-stub-runner pattern so the runner symbol
 * resolves in both HU_ENABLE_SELF_MODEL=ON and =OFF builds. */
void run_self_model_behavior_log_tests(void);
void run_action_directives_tests(void);
/* Spec 2026-05-19 self-model-scaffold — Phases B/C/D/E. Same gate
 * pattern as Phase A (stub runner under HU_ENABLE_SELF_MODEL=OFF). */
void run_self_model_phase_bcde_tests(void);
#ifdef HU_ENABLE_LEARNING
void run_w16_evaluation_tests(void);
void run_w16_eval_cli_tests(void);
#endif
void run_w15_keystore_tests(void);
void run_encrypted_store_tests(void);
#ifdef HU_ENABLE_LEARNING
void run_v2_e2e_adversarial_tests(void);
void run_v2_wiring_e2e_tests(void);
#endif
void run_b11_pressure_history_e2e_tests(void);
void run_b9_user_sim_agent_turn_e2e_tests(void);
void run_personal_model_contradicts_tests(void);
void run_channel_trust_tests(void);
void run_minja_guard_tests(void);
void run_frontier_prompt_tests(void);
void run_w11_abstain_calibration_tests(void);
void run_fast_capture_tests(void);
void run_promotion_tests(void);
void run_consolidation_tests(void);
void run_verify_claim_tests(void);
void run_deep_extract_tests(void);
void run_commitment_tests(void);
void run_contextual_proactive_tests(void);
void run_pattern_radar_tests(void);
void run_proactive_tests(void);
void run_proactive_throttle_tests(void);
void run_inner_thoughts_tests(void);
void run_weather_awareness_tests(void);
void run_timing_tests(void);
void run_calibration_tests(void);
/* run_calibration_reactions_tests + run_predictive_drafts_tests
 * forward-declared earlier (line ~95). Duplicates removed
 * 2026-05-24 — they caused both suites to run twice. */
void run_behavioral_clone_tests(void);
void run_governor_tests(void);
void run_activation_steering_tests(void);
void run_model_router_tests(void);
void run_model_router_health_tests(void);
void run_humanness_context_tests(void);
void run_turing_score_tests(void);
void run_adversarial_turing_tests(void);
void run_arbitrator_tests(void);
void run_salience_tests(void);
void run_planning_tests(void);
void run_rel_dynamics_tests(void);
void run_prospective_tests(void);
void run_prospective_memory_tests(void);
void run_emotional_residue_tests(void);
void run_consolidation_engine_tests(void);
void run_conv_goals_tests(void);
void run_knowledge_tests(void);
void run_usage_tests(void);
void run_cognitive_tests(void);
#ifdef HU_ENABLE_AUTHENTIC
void run_cognitive_load_tests(void);
void run_phase9_integration_tests(void);
#endif
void run_deep_memory_tests(void);
void run_compression_tests(void);
void run_proactive_ext_tests(void);
void run_degradation_tests(void);
void run_memory_degradation_tests(void);
void run_self_awareness_tests(void);
void run_superhuman_tests(void);
void run_contact_graph_tests(void);
void run_identity_resolver_tests(void);
void run_tool_call_parser_tests(void);
void run_tool_router_tests(void);
void run_dag_tests(void);
void run_hula_tests(void);
void run_hula_golden_tests(void);
void run_workflow_event_tests(void);
void run_sota_features_tests(void);
void run_mood_tests(void);
void run_intent_tests(void);
void run_self_uncertainty_tests(void);
void run_style_tracker_tests(void);
void run_theory_of_mind_tests(void);
void run_tom_activation_tests(void);
void run_tom_wiring_tests(void);
void run_anticipatory_tests(void);
void run_context_engine_tests(void);
void run_exec_env_tests(void);
int run_channel_monitor_tests(void);
int run_doctor_fix_tests(void);
void run_doctor_personalization_warning_tests(void);
int run_doctor_install_tests(void);
int run_skill_scaffold_tests(void);
int run_plugin_discovery_tests(void);
int run_context_engine_rag_tests(void);
int run_humanness_tests(void);
int run_opinions_persistence_tests(void);
void run_visual_content_tests(void);
void run_media_gen_tests(void);
void run_opinions_tests(void);
void run_belief_update_tests(void);
void run_taste_tests(void);
void run_somatic_tests(void);
void run_narrative_self_tests(void);
void run_attachment_tests(void);
void run_intrinsic_drive_tests(void);
void run_prosocial_routine_tests(void);
void run_life_chapters_tests(void);
void run_social_graph_tests(void);
void run_skill_system_tests(void);
void run_feeds_tests(void);
#ifdef HU_ENABLE_FEEDS
void run_apple_feeds_tests(void);
void run_news_health_email_tests(void);
#endif
#ifdef HU_ENABLE_SOCIAL
void run_social_feeds_tests(void);
#endif
#ifdef HU_ENABLE_FEEDS
void run_google_feeds_tests(void);
void run_music_feeds_tests(void);
void run_research_feeds_tests(void);
void run_research_executor_tests(void);
#endif
void run_feed_processor_tests(void);
void run_feed_awareness_tests(void);
void run_forgetting_curve_tests(void);
int run_weather_fetch_tests(void);
int run_save_for_later_tests(void);
void run_intelligence_reflection_tests(void);
void run_intelligence_skills_tests(void);
void run_skill_unified_tests(void);
void run_intelligence_cycle_tests(void);
void run_reflection_advanced_tests(void);
#ifdef HU_ENABLE_SQLITE
void run_feedback_tests(void);
#endif
void run_privacy_audit_tests(void);
void run_collab_planning_tests(void);
void run_bth_e2e_tests(void);
void run_bth_metrics_tests(void);
void run_memory_features_tests(void);
void run_agi_frontiers_tests(void);
void run_orchestrator_tests(void);
void run_swarm_execution_tests(void);
void run_dynamic_decomposition_tests(void);
void run_agent_matching_tests(void);
void run_agent_communication_tests(void);
void run_mcts_planner_tests(void);
void run_world_model_graph_tests(void);
void run_world_simulation_tests(void);
void run_world_context_tests(void);
void run_peripheral_ctrl_tests(void);
void run_value_learning_tests(void);
void run_goal_engine_tests(void);
void run_dispatch_tests(void);
void run_policy_engine_tests(void);
void run_integration_tests(void);
void run_agent_registry_tests(void);
void run_pwa_tests(void);
void run_music_tests(void);
void run_inspiration_tests(void);
void run_youtube_tests(void);
#ifdef HU_ENABLE_CURL
void run_paperclip_tests(void);
#endif
void run_cartesia_tests(void);
void run_cartesia_stream_tests(void);
void run_transcript_prep_tests(void);
void run_send_voice_message_tests(void);
void run_voice_message_integration_tests(void);
void register_voice_clone_tests(void);
#ifdef HU_ENABLE_CARTESIA
void run_audio_pipeline_tests(void);
void run_voice_decision_tests(void);
void run_emotion_map_tests(void);
#endif
#ifdef HU_ENABLE_ML
void run_ml_tests(void);
void run_mlx_admin_tests(void);
void run_ml_cli_actually_trains_tests(void);
void run_ml_fidelity_judgment_tests(void);
void run_fidelity_delta_tests(void);
/* PR #115 / merge-with-main: run_ml_cli_rl_train_tests +
 * rl_trainer_simpo + rl_trainer_orpo C tests orphaned by main's RL
 * architecture rework — files deleted, declarations removed. Sprint 12
 * FU-11.5.a will re-target against main's new framework. */
void run_dpo_judge_naming_tests(void);
void run_dp_sgd_tests(void);
void run_lora_tests(void);
void run_agent_trainer_tests(void);
void run_training_data_tests(void);
void run_training_data_extractor_tests(void);
void run_training_data_quality_tests(void);
/* Phase 2 Task 1 (RL SOTA): hu_rl_trainer_t factory dispatch pin. */
void run_rl_trainer_tests(void);
/* Phase 2 Task 2 (RL SOTA): hu_policy_logprobs sanity + determinism + null-arg. */
void run_policy_logprobs_tests(void);
/* Phase 2 Task 3 (RL SOTA): hu_reference_model_create_from clone + freeze. */
void run_reference_model_tests(void);
/* Phase 2 Task 4 (RL SOTA): real DPO loss + structural sign-of-gradient. */
void run_dpo_real_loss_tests(void);
/* Phase 2 Task 5 (RL SOTA): real DPO HUML E2E on 50 synthetic preference pairs. */
void run_dpo_real_e2e_tests(void);
/* Phase 2 Task 6 (RL SOTA): mlx-lm-lora subprocess wrapper — JSONL export
 * + dummy-adapter shortcut without HU_HAVE_MLX_LM; real Gemma DPO with it. */
void run_dpo_real_mlx_tests(void);
/* Phase 2 Task 8 (RL SOTA): pins surface contract for the post-split
 * hu_ml_cli_dpo_judge / hu_ml_cli_dpo_real CLI handlers. */
void run_cli_dpo_tests(void);
/* Phase 3 Task 1 (RL SOTA): hu_value_head_t forward + backward grad
 * check + save/load round trip. */
void run_value_head_tests(void);
/* Phase 3 Task 2 (RL SOTA): HUML reward model factory + scoring +
 * batch scoring. */
#ifdef HU_ENABLE_ML
void run_reward_model_huml_tests(void);
#else
static inline void run_reward_model_huml_tests(void) {
    (void)0;
}
#endif
/* Phase 3 Task 2 (RL SOTA): hu_reward_model_t HUML composition smoke +
 * M3 NaN contract for one-sided KTO pairs. Task 3 will APPEND
 * Bradley-Terry convergence + FD grad check to the same runner. */
void run_reward_model_train_tests(void);
/* Phase 3 Task 8 (RL SOTA): RM inference latency — HUML under budget +
 * MLX gated by HU_HAVE_MLX_LM + Qwen GGUF presence. */
void run_reward_model_inference_tests(void);
/* Phase 3 Task 5 (RL SOTA): KTO loss sign-of-gradient + finite-diff
 * grad check + vtable contract. */
void run_kto_loss_tests(void);
/* Phase 4 Task 1 (RL SOTA): KL k1/k2/k3 estimators + k3 analytical
 * backward grad — pure C leaf math primitive used by the GRPO
 * trainer (Task 5) for the KL penalty term. */
void run_kl_divergence_tests(void);
/* Phase 4 Task 2 (RL SOTA): hu_rollout_t HUML factory — sample N
 * completions per prompt with cross-platform-deterministic xorshift64
 * + per-rollout splitmix64 seed (R13). Used by GRPO trainer (Task 5)
 * to gather (token_ids, sum_logprob) for the PPO ratio clip. */
void run_rollout_tests(void);
/* Phase 4 Task 4 (RL SOTA): hu_reward_source_t leaf vtable — synthetic
 * token-counting backend (+1 per token in [1..5], -1 per token in
 * [26..30]) + Phase 3 hu_reward_model_t composition smoke + Phase 5
 * judge factory NOT_SUPPORTED pin. */
void run_reward_source_tests(void);
/* Phase 4 Task 9 (RL SOTA): `human ml grpo-train` CLI handler surface
 * contract — argument validation (R9, R12, --reward-fn/-model pairing,
 * --backend mlx demands --backbone-path) + HUML synthetic-reward e2e
 * smokes (adapter file written; --kl-beta 0 disables KL via CLI). */
void run_cli_grpo_tests(void);
#endif
void run_multigraph_tests(void);
void run_memory_graph_tests(void);
void run_experience_tests(void);
void run_experience_engine_tests(void);
void run_intelligence_wiring_tests(void);
void run_prove_e2e_tests(void);
void run_anti_sycophancy_tests(void);
void run_mutual_tom_tests(void);
void run_opinion_history_tests(void);
void run_self_improve_loop_tests(void);
void run_a2a_tests(void);
void run_gvr_tests(void);
void run_provider_degradation_tests(void);
void run_apple_provider_tests(void);
void run_escalate_tests(void);
void run_tool_validation_tests(void);
void run_data_quality_tests(void);
void run_otlp_tests(void);
void run_token_budget_tests(void);
void run_mar_tests(void);
void run_mem_policy_tests(void);
void run_prompt_optimizer_tests(void);
void run_chaos_tests(void);
void run_checkpoint_tests(void);
void run_scratchpad_tests(void);
void run_mcp_resources_tests(void);
void run_eval_tests(void);
void run_eval_judge_tests(void);
void run_eval_benchmarks_tests(void);
void run_eval_runner_tests(void);
void run_eval_history_tests(void);
void run_eval_shape_tests(void);
void run_register_tests(void);
void run_relationship_tone_tests(void);
void run_persona_head_gate_tests(void);
void run_state_file_tests(void);
void run_daemon_followup_sched_tests(void);
void run_eval_score_tests(void);
void run_corrective_rag_tests(void);
void run_adaptive_rag_tests(void);
void run_self_rag_tests(void);
void run_memory_tiers_tests(void);
void run_process_reward_tests(void);
void run_dpo_tests(void);
void run_reaction_paired_train_e2e_tests(void);
void run_dpo_collector_tests(void);
void run_proactive_outcomes_tests(void);
void run_daemon_learning_tick_tests(void);
void run_e2e_learning_loop_tests(void);
void run_sota_e2e_tests(void);
void run_sota_adversarial_tests(void);
void run_otel_tests(void);
void run_cot_audit_tests(void);
void run_moderation_tests(void);
void run_companion_safety_tests(void);
void run_code_sandbox_tests(void);
void run_computer_use_tests(void);
void run_image_gen_tests(void);
void run_visual_grounding_tests(void);
void run_browser_use_tests(void);
void run_local_voice_tests(void);
void run_gui_agent_tests(void);
void run_lsp_tests(void);
void run_webrtc_tests(void);
void run_embedded_provider_tests(void);
void run_llamacpp_provider_tests(void);
void run_llamacpp_factory_config_tests(void);
void run_llamacpp_sampling_tests(void);
void run_llamacpp_kvcache_tests(void);
void run_llamacpp_kv_quant_tests(void);
void run_llamacpp_skip_decode_tests(void);
void run_llamacpp_decode_tests(void);
void run_llamacpp_lora_hotswap_tests(void);
void run_llamacpp_chat_metal_tests(void);
void run_llamacpp_best_of_n_tests(void);
void run_doctor_best_of_n_warning_tests(void);
void run_doctor_inference_tests(void);
void run_coreml_provider_tests(void);
void run_forgetting_tests(void);
void run_bootstrap_tests(void);
void run_thread_pool_tests(void);
void run_weakness_tests(void);
void run_trust_tests(void);
void run_sota_humanness_tests(void);
void run_distiller_tests(void);
void run_plan_executor_tests(void);
void run_planner_mcts_wiring_tests(void);
void run_cdp_tests(void);
void run_emotional_cognition_tests(void);
void run_emotional_contagion_tests(void);
void run_style_mirror_tests(void);
void run_evolving_cognition_tests(void);
void run_metacognition_tests(void);
void run_humanness_frontiers_tests(void);
void run_skill_routing_tests(void);
void run_dual_process_tests(void);
void run_sota_research_tests(void);
void run_sota_wiring_tests(void);
void run_sota_live_wiring_tests(void);
void hu_test_permission(void);
void run_shell_sandbox_tests(void);
void test_session_persist(void);
void run_adversarial_memory_safety_tests(void);
void run_adversarial_injection_tests(void);
void run_adversarial_dos_protocol_tests(void);
void run_adversarial_concurrency_tests(void);
void run_adversarial_integration_tests(void);
void run_config_reload_tests(void);
void run_plugin_hooks_tests(void);
void run_approval_gate_tests(void);
void run_workflow_commands_tests(void);
void run_repair_tests(void);
void run_release_workflow_tests(void);
void run_daemon_cron_tests(void);
void run_daemon_shape_tests(void);
void run_daemon_lifecycle_tests(void);
void run_daemon_routing_tests(void);
void run_daemon_proactive_tests(void);
void run_daemon_promise_keeper_tests(void);
void run_daemon_reply_fallback_tests(void);
void run_reply_dedup_tests(void);
void run_proactive_policy_tests(void);
void run_daemon_director_tests(void);
/* Sprint 59 Phase C — test seeds feed_items via sqlite3 directly so the
 * test source is gated by HU_ENABLE_SQLITE in CMakeLists.txt. Mirror that
 * gate here so the forward decl + call site don't reference a missing
 * symbol in minimal-build / no-sqlite / cross-arm64 variants. */
#ifdef HU_ENABLE_SQLITE
void run_daemon_proactive_feed_scope_tests(void);
#endif
void run_daemon_trust_tests(void);
void run_cp_tasks_tests(void);
void run_cp_canvas_tests(void);
void run_vector_retrieval_remote_tests(void);
void run_anticipatory_state_tests(void);
void run_canvas_tool_tests(void);
void run_canvas_e2e_tests(void);
void run_canvas_persist_tests(void);
void run_canvas_render_tests(void);
void run_homebrew_formula_tests(void);
void run_background_registry_tests(void);
void run_consistency_tests(void);
void run_mlx_provider_tests(void);
void run_mlx_stream_utf8_tests(void);
void run_persona_fidelity_tests(void);
void run_persona_fidelity_judge_tests(void);
void run_persona_fidelity_validator_tests(void);
void run_persona_voice_validator_tests(void);
void run_identity_short_circuit_validator_tests(void);
void run_persona_fidelity_cross_tests(void);
#ifdef HU_ENABLE_ML
void run_dpo_extractor_integration_tests(void);
#endif
void run_fact_extract_llm_tests(void);
void run_fact_extract_tests(void);
void run_personal_model_tests(void);
void run_personal_model_llm_extract_tests(void);
void run_personal_model_atomic_save_tests(void);
void run_personal_model_per_contact_tests(void);
#ifdef HU_ENABLE_SQLITE
void run_cross_channel_acl_tests(void);
void run_cross_channel_pipeline_tests(void);
void run_reflection_schema_tests(void);
#endif
void run_reflection_storage_tests(void);                 /* T2: stub when SQLite off */
void run_reflection_prompt_tests(void);                  /* T4: stub when SQLite off */
void run_reflection_orchestration_tests(void);           /* T5: stub when SQLite off */
void run_reflection_consumer_tests(void);                /* T6: stub when SQLite off */
void run_reflection_turn_source_tests(void);             /* T9fu: stub when SQLite off */
void run_personal_model_reflection_slice_tests(void);    /* T7: stub when SQLite off */
void run_reflection_retire_on_contradiction_tests(void); /* T8: stub when SQLite off */
void run_reflection_quorum_tests(void);                  /* T11: stub when SQLite off */
void run_reflection_e2e_tests(void);                     /* T10: stub when SQLite off */
void run_doctor_reflection_loop_tests(void);             /* T12: stub when SQLite off */
void run_emotional_context_tests(void);
void run_autoresponder_tests(void);
void run_autoresponder_eval_tests(void);
void run_contact_narrative_tests(void);
void run_causal_attribution_tests(void);
void run_identity_continuity_tests(void);
void run_audio_emotion_tests(void);
void run_style_adapter_tests(void);
void run_lora_export_tests(void);
void run_lora_nightly_tests(void);
void run_adapter_swap_tests(void);
void run_lora_subprocess_tests(void);
void run_style_critique_patterns_tests(void);
void run_style_self_critique_tests(void);
void run_personal_model_simulation_tests(void);
#ifdef HU_ENABLE_RL_FULL
void run_personal_model_fidelity_v2_tests(void);
void run_grpo_loss_tests(void);
void run_grpo_mlx_tests(void);
void run_grpo_huml_tests(void);
void run_grpo_e2e_tests(void);
#endif
#ifdef HU_HAS_LIBSODIUM
void run_persona_encryption_tests(void);
#endif
void run_persona_directive_channels_tests(void);
void run_persona_overlay_render_tests(void);
#if defined(HU_HAS_IMESSAGE) && defined(HU_HAS_TELEGRAM)
void run_channel_overlay_apply_tests(void);
#endif
void run_filler_recency_tests(void);
void run_contact_send_recency_tests(void);
#ifdef HU_ENABLE_ML
void run_dpo_miner_tests(void);
#endif
void run_sprint3_hybrid_recall_tests(void);
/* PR #115: 3 declarations + calls removed — see CMakeLists.txt comment
 * at line ~2970. These tests were ghost-registered (declared in
 * test_main.c but not compiled pre-PR) and exercise features not yet
 * fully implemented:
 *   - run_config_identity_links_tests
 *   - run_memory_session_scoping_tests
 *   - run_imessage_outbound_dedup_tests
 * Activated briefly during merge resolution; produced 11 failures in
 * minimal-build (8 from missing parsers + 3 from cross-test state
 * pollution on the dedup channel). Re-add when the underlying
 * features land. */
void run_filler_pctt_tests(void);
void run_hallucination_guard_tests(void);
void run_humor_fw_tests(void);
void run_self_improve_tests(void);
void run_sycophancy_guard_tests(void);
void run_trust_calibration_tests(void);
void run_vision_ocr_tests(void);
void run_markdown_loader_tests(void);
void run_structured_output_tests(void);
#ifdef HU_ENABLE_RL_FULL
extern void run_bootstrap_ci_tests(void);
extern void run_eval_judge_external_tests(void);
extern void run_leaderboard_tests(void);
extern void run_eval_gate_tests(void);
extern void run_stock_baseline_tests(void);
extern void run_apple_fm_client_tests(void);
extern void run_gemini_nano_client_tests(void);
extern void run_competitive_harness_tests(void);
extern void run_cli_eval_phase5_tests(void);
extern void run_cli_demo_evidence_tests(void);
extern void run_lora_ab_require_positive_tests(void);
extern void run_runner_eval_gate_tests(void);
extern void run_daemon_reaction_poll_tests(void);
extern void run_proof_directory_tests(void);
extern void run_e2e_closed_loop_tests(void);
#endif

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("  --suite=<name>   Run only suites whose name contains <name>\n");
    printf("  --filter=<name>  Run only tests whose function name contains <name>\n");
    printf("  --flaky-retries=<n>  Extra attempts for HU_RUN_TEST_FLAKY tests "
           "(default 2; 0 disables; env HU_TEST_FLAKY_RETRIES)\n");
    printf("  --help           Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s --suite=config          # run config-related suites\n", prog);
    printf("  %s --filter=json_parse     # run tests matching 'json_parse'\n", prog);
    printf("  %s --suite=security --filter=vault  # combine both\n", prog);
}

/* Phase 1 (RL SOTA) — declared in src/providers/llamacpp.c when
 * HU_LLAMACPP_LINKED is set; provides a one-shot CLI that loads a
 * GGUF, decodes one chat turn, and prints the response. Used by
 * scripts/run-gemma-sanity-gate.sh to score the 20-prompt fixture. */
#ifdef HU_ENABLE_LLAMACPP
int hu_llamacpp_sanity_gate_main(int argc, char **argv);
#endif

int main(int argc, char **argv) {
    /* Line-buffer stdout even when piped (non-TTY stdout is fully buffered by
     * default), so the per-suite progress and the final "Results:" line reach
     * the pipe/log BEFORE any end-of-process LeakSanitizer abort discards the
     * unflushed tail. Done here instead of wrapping the binary in `stdbuf`:
     * stdbuf injects libstdbuf.so via LD_PRELOAD, which displaces the ASan
     * runtime from the front of the initial library list and makes ASan-built
     * binaries abort at startup on Linux (broke the RL nightly 05-31..07-25). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc >= 2 && strcmp(argv[1], "--sanity-gate") == 0) {
#ifdef HU_ENABLE_LLAMACPP
        return hu_llamacpp_sanity_gate_main(argc, argv);
#else
        fprintf(stderr,
                "[sanity-gate] HU_ENABLE_LLAMACPP not set; rebuild with --preset rl_sota\n");
        return 1;
#endif
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--suite=", 8) == 0) {
            hu__suite_filter = argv[i] + 8;
        } else if (strncmp(argv[i], "--filter=", 9) == 0) {
            hu__test_filter = argv[i] + 9;
        } else if (strncmp(argv[i], "--flaky-retries=", 16) == 0) {
            hu__flaky_retries = atoi(argv[i] + 16);
            if (hu__flaky_retries < 0)
                hu__flaky_retries = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* HU_TEST_FLAKY_RETRIES overrides the default (2); --flaky-retries= wins
     * over both. 0 disables retry (HU_RUN_TEST_FLAKY behaves like HU_RUN_TEST). */
    {
        const char *fr = getenv("HU_TEST_FLAKY_RETRIES");
        int cli_set = 0;
        for (int i = 1; i < argc; i++)
            if (strncmp(argv[i], "--flaky-retries=", 16) == 0)
                cli_set = 1;
        if (!cli_set && fr && *fr) {
            int v = atoi(fr);
            hu__flaky_retries = v < 0 ? 0 : v;
        }
    }

    printf("Human Test Suite\n");
    fflush(stdout);
    printf("==================\n");
    if (hu__suite_filter)
        printf("Suite filter: %s\n", hu__suite_filter);
    if (hu__test_filter)
        printf("Test filter:  %s\n", hu__test_filter);
    if (hu__flaky_retries != 2)
        printf("Flaky retries: %d\n", hu__flaky_retries);

    run_flaky_harness_tests();
    run_allocator_tests();
    run_data_loader_tests();
    run_idempotency_tests();
    run_idempotency_hula_integration_tests();
    run_agent_modules_tests();
    run_agent_definition_tests();
    run_agent_git_tests();
    run_agent_app_config_tests();
    run_task_store_tests();
    run_compaction_hierarchical_tests();
    run_tot_recursive_tests();
    run_agent_subsystems_tests();
    run_crypto_tests();
    run_wasm_tests();
    run_json_tests();
    run_string_tests();
    run_string_ci_tests();
    run_rand_tests();
    run_log_once_tests();
    run_vertex_adc_tests();
    run_init_proposer_tests();
    run_init_proposer_compose_tests();
    run_init_outcome_tests();
    run_init_dpo_bridge_tests();
    run_prompt_budget_tests();
    run_prompt_budget_snapshot_tests();
    run_config_gated_subsystems_tests();
    run_silent_disable_compliance_tests();
    run_io_secure_tests();
    run_file_slurp_tests();
    run_config_banner_runtime_failfast_tests();
    run_slice_tests();
    run_tool_registry_honesty_tests();
    run_contextual_bandit_tests();
    run_humanization_bandit_tests();
    run_memory_tests();
    run_w7_render_null_safety_tests();
    run_mlx_load_adapter_tests();
    run_m3_route_per_turn_tests();
    run_m3_route_per_turn_call_sites_tests();
    run_m3_swap_failure_observability_tests();
    run_m3_outcome_ring_population_tests();
    run_m3_ab_fidelity_gate_tests();
    run_m3_frontier_auto_invocation_tests();
    run_sql_transaction_tests();
    run_sqlite_integrity_tests();
    run_memory_util_tests();
    run_tunnel_tests();
    run_gateway_tests();
    run_auth_tests();
    run_oauth_tests();
    run_security_tests();
    run_normalize_tests();
    run_sensitivity_tests();
    run_vault_tests();
    run_vault_aead_tests();
    run_app_bundle_structure_tests();
    run_pkg_builder_tests();
    run_install_docs_tests();
    run_sign_notarize_tests();
    run_provider_tests();
    run_provider_http_tests();
    run_gemini_vertex_auth_tests();
    run_ensemble_tests();
    run_api_key_tests();
    run_channel_tests();
    run_channel_class_tests();
    run_channel_format_tests();
    run_channel_behavior_class_tests();
    run_channel_rate_limit_tests();
    run_channel_http_tests();
    run_webhook_channel_tests();
    run_bg_registry_tests();
    run_channel_embeds_tests();
    run_reaction_event_tests();
    run_imessage_reactions_tests();
    run_imessage_caps_tests();
    run_imessage_bb_event_tests();
    run_imessage_emoji_reaction_tests();
    run_imessage_ingest_tests();
    run_imessage_personal_model_e2e_tests();
    run_bplist_tests();
    run_imessage_observer_tests();
    run_imessage_schema_tests();
    run_typedstream_tests();
    run_harmony_filter_tests();
    run_imessage_balloon_decode_tests();
    run_whatsapp_reactions_tests();
    run_matrix_reactions_tests();
    run_reaction_handler_lookup_store_tests();
    run_imessage_gaps_tests();
    run_pattern_drift_tests();
    run_contact_signature_tests();
    run_persona_social_insights_tests();
    run_daemon_social_tick_tests();
    run_personal_model_topics_from_reactions_tests();
    run_calibration_reactions_tests();
    run_predictive_drafts_tests();
    run_discord_reactions_tests();
    run_telegram_reactions_tests();
    run_slack_reactions_tests();
#ifdef HU_ENABLE_SQLITE
    run_reaction_handler_e2e_tests();
#endif
    run_declarative_tools_tests();
    run_skill_trust_tests();
    run_tool_tests();
    run_webhook_tests();
    test_vtables_run();
    run_peripheral_tests();
    run_peripheral_ctrl_tests();
    run_value_learning_tests();
    run_goal_engine_tests();
    run_dispatch_tests();
    run_policy_engine_tests();
    run_e2e_tests();
    run_integration_tests();
    run_e2e_conversation_tests();
    run_e2e_agent_loop_tests();
    run_subsystems_tests();
    run_onboard_state_tests();
    run_onboard_dispatcher_tests();
    run_onboard_step1_tests();
    run_onboard_nextstep_tests();
    run_onboard_aloop_tests();
    run_config_parse_tests();
    run_config_migrate_tests();
    run_adversarial_tests();
    run_adversarial_detect_tests();
    run_gateway_http_tests();
    run_memory_full_tests();
    run_tools_all_tests();
    run_diagnose_notary_tests();
    run_rag_tests();
    run_multimodal_tests();
    run_multimodal_pipeline_tests();
    run_multimodal_memory_tests();
    run_multimodal_audio_tests();
    run_multimodal_video_tests();
    run_voice_duplex_tests();
    run_turn_signal_tests();
    run_voice_rt_openai_tests();
    run_voice_provider_tests();
    run_voice_session_tests();
    run_gemini_live_tests();
    run_voice_factory_e2e_tests();
    run_voice_streaming_e2e_tests();
    run_mlx_local_voice_tests();
    run_autonomy_tests();
    run_retrieval_tests();
    run_retrieval_contact_isolation_tests();
    run_multigraph_tests();
    run_memory_graph_tests();
    run_vector_tests();
    run_vector_full_tests();
    run_infrastructure_tests();
    run_memory_subsystems_tests();
    run_http_tests();
    run_sse_tests();
    run_streaming_tests();
    run_websocket_tests();
    run_ws_integration_tests();
    run_net_security_tests();
    run_path_security_tests();
    run_process_util_tests();
    run_prompt_tests();
    run_prompt_trim_tests();
    run_gate_mode_tests();
    run_graph_grounding_tests();
    run_uncertainty_tests();
    run_tool_search_tests();
    run_persona_tests();
    run_terseness_tests();
    run_circadian_tests();
    run_relationship_tests();
    run_replay_tests();
    run_style_clone_tests();
    run_uncertainty_tests();
    run_life_sim_tests();
    run_persona_mood_tests();
    run_persona_feedback_tests();
    run_persona_examples_style_tests();
    run_persona_filler_roundtrip_tests();
    run_persona_cli_tests();
    run_persona_sticker_tests();
    run_voice_maturity_tests();
    run_style_mirror_tests();
    run_style_learner_tests();
    run_persona_refresh_tests();
    run_persona_rag_tests();
    run_temporal_tests();
    run_inner_world_tests();
    run_persona_eval_tests();
    /* Moment Context Decision Layer (Task 0.2) — empty suites until Phase 1/2. */
    run_moment_compose_tests();
    run_moment_render_tests();
    run_behavior_policy_tests();
    run_behavior_dialog_act_tests();
    run_behavior_affect_tests();
    run_behavior_change_tests();
    run_behavior_safety_tests();
    run_behavior_prosocial_tests();
    run_win_detect_tests();
    run_celebration_tests();
    run_prosocial_moment_tests();
#ifdef HU_ENABLE_SQLITE
    run_celebration_repo_tests();
#endif
    run_behavior_prompt_tests();
    run_behavior_support_strategy_tests();
    run_behavior_trust_tests();
    run_tapback_band_tests();
    run_behavior_corpora_tests();
    run_user_sim_tests();
    run_tom_scenario_tests();
    run_behavior_trust_prompt_tests();
    run_behavior_pressure_tests();
    run_sycophancy_pack_tests();
    run_longmemeval_tests();
    run_user_sim_scenario_tests();
    run_chronotype_tests();
    run_lifecycle_tests();
    run_observer_tests();
    run_session_tests();
    run_bus_tests();
    run_identity_tests();
    run_channel_manager_tests();
    run_new_modules_tests();
    run_provider_all_tests();
    run_chat_response_diag_tests();
    run_channel_all_tests();
    run_meta_common_tests();
    run_channel_integration_tests();
    run_channel_vtable_action_surface_tests();
    run_config_extended_tests();
    run_config_getters_tests();
    run_config_validation_tests();
    run_config_action_surface_tests();
    run_config_seth_voice_defaults_tests();
    run_json_extended_tests();
    run_security_extended_tests();
    run_security_pipeline_tests();
    run_core_extended_tests();
    run_gateway_extended_tests();
    run_gateway_auth_tests();
    run_gateway_voice_tests();
    run_gateway_hula_traces_tests();
    run_pairing_tests();
    run_agent_extended_tests();
    run_agent_security_tests();
    run_agent_teams_tests();
    run_delegation_tests();
    run_diagnostic_commands_tests();
    run_skills_tests();
    run_memory_new_tests();
    run_ported_modules_tests();
    run_doctor_imessage_diagnose_tests();
    run_doctor_registry_tests();
    run_doctor_chatdb_tests();
    run_doctor_check_provider_tests();
    run_doctor_exit_codes_tests();
    run_doctor_json_output_tests();
    run_doctor_reaction_collection_wired_tests();
    run_doctor_prompt_budget_tests();
    run_outbound_sanitize_tests();
    run_daemon_follow_up_watcher_tests();
    run_cli_ctl_tests();
    run_doctor_local_voice_tests();
    run_onboard_step_provider_tests();
    run_cron_tests();
    run_cron_session_tools_tests();
    run_subagent_tests();
    run_mcp_tests();
    run_mcp_jsonrpc_tests();
    run_mcp_manager_tests();
    run_mcp_resource_tools_tests();
    run_mcp_transport_tests();
    run_mcp_transport_sse_tests();
    run_mcp_http_integration_tests();
    run_otel_trace_tests();
    run_mcp_audit_tests();
    run_voice_tests();
    run_vector_stores_tests();
    run_cli_tests();
    run_update_tests();
    run_memory_engines_ext_tests();
    run_memory_poisoning_tests();
    run_runtime_tests();
    run_runtime_bundle_tests();
    run_channel_loop_tests();
    run_util_modules_tests();
    run_roadmap_tests();
    run_new_features_tests();
    run_ollama_integration_tests();
    run_plugin_tests();
    run_tenant_tests();
    run_gmail_tests();
    run_imessage_extended_tests();
    run_imessage_reply_style_tests();
    run_imessage_reply_fallback_quote_tests();
    run_imessage_private_protocol_tests();
    run_imessage_private_client_tests();
    run_imessage_chatdb_fixture_tests();
    run_imessage_adversarial_tests();
    run_imessage_non_allowlisted_tests();
    run_imessage_rich_link_tests();
    run_imessage_react_contract_tests();
    run_imessage_reply_pacing_tests();
    run_imessage_action_telemetry_tests();
    run_follow_up_tests();
    run_imessage_action_telemetry_tests();
    run_imessage_action_telemetry_tests();
    run_imessage_reply_pacing_tests();
    run_imessage_threaded_reply_tests();
    run_imessage_custom_tapback_tests();
    run_imessage_action_facts_tests();
    run_imessage_dispatcher_tests();
    run_imessage_sticker_tests();
    run_follow_up_tests();
    run_follow_up_daemon_integration_tests();
    run_daemon_aloop_smoke_tests();
    run_intelligence_tests();
    run_protective_tests();
    run_humor_tests();
    run_authentic_tests();
    run_rag_pipeline_tests();
    run_persona_training_tests();
    run_behavioral_tests();
    run_context_ext_tests();
    run_untested_modules_tests();
    run_modules_coverage_tests();
    run_coverage_new_tests();
    run_context_tests();
    run_qmd_tests();
    run_terminal_tests();
    run_tavily_tests();
    run_awareness_tests();
    run_entropy_gate_tests();
    run_episodic_tests();
    run_reflection_tests();
    run_input_guard_tests();
    run_externalization_tests();
    run_conversation_tests();
    run_vision_tests();
    run_ab_response_tests();
    run_event_extract_tests();
    run_stm_tests();
    run_emotional_graph_tests();
    run_comfort_patterns_tests();
    run_emotional_moments_tests();
    run_emotional_state_tests();
    run_contact_style_overlay_tests();
    run_graph_tests();
    run_w1_bitemporal_tests();
    run_w2_autodream_tests();
    run_w3_multigraph_tests();
    run_w4_verifier_tests();
    run_w5_persona_deltas_tests();
    run_persona_delta_observer_tests();
    run_world_model_bridge_tests();
    run_signal_channel_wire_tests();
    run_daemon_housekeeping_tests();
    run_orphan_channel_audit_tests();
    run_verifier_metrics_tests();
    run_doctor_ws_consumer_tests();
    run_output_validator_tests();
    run_chain_failure_paths_tests();
    run_agent_fail_path_regressions_tests();
    run_stop_sequences_tests();
    run_validators_builtin_tests();
    run_pattern_c_paths_tests();
    run_validators_persona_safety_tests();
    run_validator_reject_discards_tests();
    run_validator_telemetry_tests();
    run_validator_chain_cache_tests();
    run_daemon_e2e_validator_tests();
    run_response_guard_tests();
    run_response_guard_retry_tests();
    run_outbound_pipeline_tests();
    run_outbound_strip_tests();
    run_style_governor_tests();
    run_outbound_shape_tests();
    run_outbound_echo_tests();
    run_outbound_crosstalk_tests();
#ifdef HU_ENABLE_SQLITE
    run_boundary_repo_tests();
    run_opinions_repo_tests();
    run_life_chapter_repo_tests();
    run_social_graph_repo_tests();
    run_self_awareness_repo_tests();
    run_feed_items_repo_tests();
    run_memories_repo_tests();
    run_emotional_moments_repo_tests();
    run_emotional_residue_repo_tests();
    run_emotional_state_repo_tests();
    run_mood_repo_tests();
    run_self_model_repo_tests();
    run_theory_of_mind_repo_tests();
    run_outbound_crosstalk_sqlite_tests();
    run_outbound_e2e_sota_proof_tests();
    run_burst_egress_tests();
    run_outbound_stats_e2e_tests();
    run_outbound_pipeline_perf_tests();
#endif
    run_outbound_persona_tests();
    run_outbound_persona_classifier_tests();
    run_outbound_moderation_tests();
    run_outbound_corpus_regression_tests();
    run_outbound_stats_tests();
    run_doctor_outbound_stats_tests();
    run_doctor_unified_dispatch_tests();
    run_multimodal_policy_tests();
    run_persona_eval_tests();
    /* Sprint 46 R5.3 carryover (audit FAIL fix) — agent integration tests */
    run_agent_tests();
    /* #26: per-turn state tracking unit tests (tool_count, hash, registers) */
    run_agent_turn_state_tests();
    /* M4 follow-up: transport-error fast-fail in agent_turn tool-loop */
    run_agent_turn_transport_tests();
    /* G11: per-turn request override parity helper (G5 regression guard) */
    run_agent_turn_request_overrides_tests();
    run_w6_e2e_adversarial_tests();
    run_w7_memory_facade_tests();
    run_w8_belief_layer_tests();
    run_w9_world_model_tests();
    run_w10_neural_memory_tests();
    run_w11_self_rag_tests();
    run_w12_planner_tests();
    run_w12_verifier_loop_tests();
#ifdef HU_ENABLE_LEARNING
    run_w13_learner_tests();
    run_w14_runners_tests();
    run_w14_lora_retrain_tests();
    run_w14_dual_lora_tests();
    run_learner_bridge_tests();
#endif
    run_w15_backup_restore_tests();
    run_w14_scheduler_tests();
    /* Spec 2026-05-19 — DPO pair-count auto-training trigger. */
    run_dpo_pair_count_trigger_tests();
    run_training_runner_shared_entry_tests();
    /* US-8 / M3 frontier-MLX dispatch via training_loop.py subprocess. */
    run_m3_frontier_mlx_dispatch_tests();
    /* Spec 2026-05-19 self-model-scaffold — runs in both flag variants. */
    run_self_model_behavior_log_tests();
    run_action_directives_tests();
    run_self_model_phase_bcde_tests();
#ifdef HU_ENABLE_LEARNING
    run_w16_evaluation_tests();
    run_w16_eval_cli_tests();
#endif
    run_w15_keystore_tests();
    run_encrypted_store_tests();
#ifdef HU_ENABLE_LEARNING
    run_v2_e2e_adversarial_tests();
    run_v2_wiring_e2e_tests();
#endif
    run_b11_pressure_history_e2e_tests();
    run_b9_user_sim_agent_turn_e2e_tests();
    run_personal_model_contradicts_tests();
    run_channel_trust_tests();
    run_minja_guard_tests();
    run_frontier_prompt_tests();
    run_w11_abstain_calibration_tests();
    run_fast_capture_tests();
    run_promotion_tests();
    run_consolidation_tests();
    run_verify_claim_tests();
    run_deep_extract_tests();
    run_commitment_tests();
    run_contextual_proactive_tests();
    run_pattern_radar_tests();
    run_proactive_tests();
    run_proactive_throttle_tests();
    run_inner_thoughts_tests();
    run_weather_awareness_tests();
    run_timing_tests();
    run_calibration_tests();
    /* run_calibration_reactions_tests + run_predictive_drafts_tests
     * called earlier (line ~881). Duplicates removed 2026-05-24. */
    run_behavioral_clone_tests();
    run_governor_tests();
    run_activation_steering_tests();
    run_model_router_tests();
    run_model_router_health_tests();
    run_humanness_context_tests();
    run_turing_score_tests();
    run_adversarial_turing_tests();
    run_arbitrator_tests();
    run_salience_tests();
    run_planning_tests();
    run_rel_dynamics_tests();
#ifdef HU_ENABLE_SQLITE
    run_prospective_tests();
    run_prospective_memory_tests();
    run_emotional_residue_tests();
    run_consolidation_engine_tests();
#endif
    run_conv_goals_tests();
    run_knowledge_tests();
    run_usage_tests();
    run_cognitive_tests();
#ifdef HU_ENABLE_AUTHENTIC
    run_cognitive_load_tests();
    run_phase9_integration_tests();
#endif
    run_deep_memory_tests();
    run_compression_tests();
    run_proactive_ext_tests();
    run_degradation_tests();
    run_memory_degradation_tests();
    run_self_awareness_tests();
    run_superhuman_tests();
    run_contact_graph_tests();
    run_identity_resolver_tests();
    run_tool_call_parser_tests();
    run_tool_router_tests();
    run_dag_tests();
    run_hula_tests();
    run_hula_golden_tests();
    run_workflow_event_tests();
    run_sota_features_tests();
    run_mood_tests();
    run_intent_tests();
    run_self_uncertainty_tests();
    run_style_tracker_tests();
    run_theory_of_mind_tests();
    run_tom_activation_tests();
    run_tom_wiring_tests();
    run_anticipatory_tests();
    run_context_engine_tests();
    run_exec_env_tests();
    run_channel_monitor_tests();
    run_doctor_fix_tests();
    run_doctor_personalization_warning_tests();
    run_doctor_install_tests();
    run_skill_scaffold_tests();
    run_plugin_discovery_tests();
    run_context_engine_rag_tests();
    run_humanness_tests();
    run_opinions_persistence_tests();
    run_visual_content_tests();
    run_media_gen_tests();
    run_opinions_tests();
    run_belief_update_tests();
    run_taste_tests();
    run_somatic_tests();
    run_narrative_self_tests();
    run_attachment_tests();
    run_intrinsic_drive_tests();
    run_prosocial_routine_tests();
    run_life_chapters_tests();
    run_social_graph_tests();
    run_skill_system_tests();
    run_feeds_tests();
#ifdef HU_ENABLE_FEEDS
    run_apple_feeds_tests();
    run_news_health_email_tests();
    run_google_feeds_tests();
    run_music_feeds_tests();
    run_research_feeds_tests();
    run_research_executor_tests();
#endif
#ifdef HU_ENABLE_SOCIAL
    run_social_feeds_tests();
#endif
    run_feed_processor_tests();
    run_feed_awareness_tests();
    run_forgetting_curve_tests();
    run_weather_fetch_tests();
    run_save_for_later_tests();
    run_intelligence_reflection_tests();
    run_intelligence_skills_tests();
    run_skill_unified_tests();
    run_intelligence_cycle_tests();
    run_reflection_advanced_tests();
#ifdef HU_ENABLE_SQLITE
    run_feedback_tests();
#endif
    run_privacy_audit_tests();
    run_collab_planning_tests();
    run_bth_e2e_tests();
    run_bth_metrics_tests();
    run_memory_features_tests();
    run_agi_frontiers_tests();
    run_orchestrator_tests();
    run_swarm_execution_tests();
    run_dynamic_decomposition_tests();
    run_agent_matching_tests();
    run_agent_communication_tests();
    run_mcts_planner_tests();
    run_world_model_graph_tests();
    run_world_simulation_tests();
    run_world_context_tests();
    run_agent_registry_tests();
    run_pwa_tests();
    run_music_tests();
    run_inspiration_tests();
    run_youtube_tests();
#ifdef HU_ENABLE_CURL
    run_paperclip_tests();
#endif
    run_cartesia_tests();
    run_cartesia_stream_tests();
    run_transcript_prep_tests();
    run_send_voice_message_tests();
    run_voice_message_integration_tests();
    register_voice_clone_tests();
#ifdef HU_ENABLE_CARTESIA
    run_audio_pipeline_tests();
    run_voice_decision_tests();
    run_emotion_map_tests();
#endif
#ifdef HU_ENABLE_ML
    run_ml_tests();
    run_mlx_admin_tests();
    run_ml_cli_actually_trains_tests();
    run_ml_fidelity_judgment_tests();
    run_fidelity_delta_tests();
    /* PR #115 / merge-with-main: run_ml_cli_rl_train_tests +
     * rl_trainer_orpo/simpo suites removed — they pinned Sprint 11's
     * per-trainer factory pattern, orphaned by main's RL architecture
     * rework. See declaration block at ~line 441. */
    run_dpo_judge_naming_tests();
    run_dp_sgd_tests();
    run_lora_tests();
    run_agent_trainer_tests();
    run_training_data_tests();
    run_training_data_extractor_tests();
    run_training_data_quality_tests();
    /* Phase 2 Task 1 (RL SOTA): hu_rl_trainer_t factory dispatch pin. */
    run_rl_trainer_tests();
    /* Phase 2 Task 2 (RL SOTA): hu_policy_logprobs sanity + determinism + null-arg. */
    run_policy_logprobs_tests();
    /* Phase 2 Task 3 (RL SOTA): hu_reference_model_create_from clone + freeze. */
    run_reference_model_tests();
    /* Phase 2 Task 4 (RL SOTA): real DPO loss + structural sign-of-gradient. */
    run_dpo_real_loss_tests();
#ifdef HU_ENABLE_RL_FULL
    /* Phase 2 Task 5 (RL SOTA): real DPO HUML E2E on 50 synthetic preference pairs. */
    run_dpo_real_e2e_tests();
    /* Phase 2 Task 6 (RL SOTA): mlx-lm-lora subprocess wrapper test (skip stub
     * by default; real Gemma DPO when HU_HAVE_MLX_LM=1). */
    run_dpo_real_mlx_tests();
    /* Phase 2 Task 8 (RL SOTA): post-split CLI handler surface contract. */
    run_cli_dpo_tests();
#endif
    /* Phase 3 Task 1 (RL SOTA): hu_value_head_t linear projection — forward,
     * backward (analytical + finite-diff grad check), save/load round trip. */
    run_value_head_tests();
    /* Phase 3 Task 2 (RL SOTA): HUML reward model factory + scoring. */
    run_reward_model_huml_tests();
    /* Phase 3 Task 2 (RL SOTA): hu_reward_model_t vtable + HUML factory
     * (toy GPT + Task 1 value head). Smoke score-returns-finite + M3
     * NaN contract for one-sided KTO pairs in score_batch. */
    run_reward_model_train_tests();
    /* Phase 3 Task 8 (RL SOTA): RM inference latency tests. */
    run_reward_model_inference_tests();
#ifdef HU_ENABLE_RL_FULL
    /* Phase 3 Task 5 (RL SOTA): KTO loss. */
    run_kto_loss_tests();
#endif
    /* Phase 4 Task 1 (RL SOTA): KL k1/k2/k3 + k3 backward grad. */
    run_kl_divergence_tests();
    /* Phase 4 Task 2 (RL SOTA): hu_rollout_t HUML — sampling determinism
     * + per-rollout splitmix64 PRNG (R13 cross-platform pin). */
    run_rollout_tests();
    /* Phase 4 Task 4 (RL SOTA): hu_reward_source_t — synthetic token
     * counting + Phase 3 RM composition + Phase 5 judge stub pin. */
    run_reward_source_tests();
#ifdef HU_ENABLE_RL_FULL
    /* Phase 4 Task 9 (RL SOTA): `human ml grpo-train` CLI handler. */
    run_cli_grpo_tests();
#endif
#endif

    run_experience_tests();
    run_experience_engine_tests();
    run_intelligence_wiring_tests();
    run_prove_e2e_tests();
    run_anti_sycophancy_tests();
    run_mutual_tom_tests();
    run_opinion_history_tests();
    run_self_improve_loop_tests();
    run_a2a_tests();
    run_gvr_tests();
    run_provider_degradation_tests();
    run_apple_provider_tests();
    run_escalate_tests();
    run_tool_validation_tests();
    run_data_quality_tests();
    run_otlp_tests();
    run_token_budget_tests();
    run_mar_tests();
    run_mem_policy_tests();
    run_prompt_optimizer_tests();
    run_chaos_tests();
    run_checkpoint_tests();
    run_scratchpad_tests();
    run_mcp_resources_tests();
    run_eval_tests();
    run_eval_judge_tests();
    run_eval_benchmarks_tests();
    run_eval_runner_tests();
    run_eval_history_tests();
    run_eval_shape_tests();
    run_register_tests();
    run_relationship_tone_tests();
    run_persona_head_gate_tests();
    run_state_file_tests();
    run_daemon_followup_sched_tests();
    run_eval_score_tests();
    run_corrective_rag_tests();
    run_adaptive_rag_tests();
    run_self_rag_tests();
    run_memory_tiers_tests();
    run_process_reward_tests();
    run_dpo_tests();
    run_reaction_paired_train_e2e_tests();
    run_dpo_collector_tests();
    run_proactive_outcomes_tests();
    run_daemon_learning_tick_tests();
    run_e2e_learning_loop_tests();
    run_sota_e2e_tests();
    run_sota_adversarial_tests();
    run_otel_tests();
    run_cot_audit_tests();
    run_moderation_tests();
    run_companion_safety_tests();
    run_code_sandbox_tests();
    run_computer_use_tests();
    run_image_gen_tests();
    run_visual_grounding_tests();
    run_browser_use_tests();
    run_local_voice_tests();
    run_gui_agent_tests();
    run_lsp_tests();
    run_webrtc_tests();
    run_embedded_provider_tests();
    run_llamacpp_provider_tests();
    run_llamacpp_factory_config_tests();
    run_llamacpp_sampling_tests();
    run_llamacpp_kvcache_tests();
    run_llamacpp_kv_quant_tests();
    run_llamacpp_skip_decode_tests();
    run_llamacpp_decode_tests();
    run_llamacpp_lora_hotswap_tests();
    run_llamacpp_chat_metal_tests();
    run_llamacpp_best_of_n_tests();
    run_doctor_best_of_n_warning_tests();
    run_doctor_inference_tests();
    run_coreml_provider_tests();
    run_forgetting_tests();
    run_bootstrap_tests();
    run_thread_pool_tests();
    run_weakness_tests();
    run_trust_tests();
    run_sota_humanness_tests();
    run_distiller_tests();
    run_plan_executor_tests();
    run_planner_mcts_wiring_tests();
    run_cdp_tests();
    run_emotional_cognition_tests();
    run_emotional_contagion_tests();
    run_evolving_cognition_tests();
    run_metacognition_tests();
    run_humanness_frontiers_tests();
    run_skill_routing_tests();
    run_dual_process_tests();
    run_sota_research_tests();
    run_sota_wiring_tests();
    run_sota_live_wiring_tests();
    hu_test_permission();
    run_shell_sandbox_tests();
    run_hook_pipeline_tests();
    run_agent_dispatch_hooks_tests();
    test_session_persist();
    run_compaction_structured_tests();
    run_instruction_discover_tests();
    run_adversarial_memory_safety_tests();
    run_adversarial_injection_tests();
    run_adversarial_dos_protocol_tests();
    run_adversarial_concurrency_tests();
    run_adversarial_integration_tests();
    run_config_reload_tests();
    run_plugin_hooks_tests();
    run_task_manager_tests();
    run_task_tools_tests();
    run_tool_ask_user_tests();
    run_approval_gate_tests();
    run_workflow_commands_tests();
    run_repair_tests();
    run_release_workflow_tests();
    run_daemon_cron_tests();
    run_daemon_shape_tests();
    run_daemon_lifecycle_tests();
    run_daemon_routing_tests();
    run_daemon_proactive_tests();
    run_daemon_promise_keeper_tests();
    run_daemon_reply_fallback_tests();
    run_reply_dedup_tests();
    run_proactive_policy_tests();
    run_daemon_director_tests();
#ifdef HU_ENABLE_SQLITE
    run_daemon_proactive_feed_scope_tests();
#endif
    run_daemon_trust_tests();
    run_cp_tasks_tests();
    run_cp_canvas_tests();
    run_vector_retrieval_remote_tests();
    run_background_registry_tests();
    run_consistency_tests();
    run_mlx_provider_tests();
    run_mlx_stream_utf8_tests();
    run_persona_fidelity_tests();
    run_persona_fidelity_judge_tests();
    run_persona_fidelity_validator_tests();
    run_persona_voice_validator_tests();
    run_identity_short_circuit_validator_tests();
    run_persona_fidelity_cross_tests();
#ifdef HU_ENABLE_ML
    run_dpo_extractor_integration_tests();
#endif
    run_fact_extract_llm_tests();
    run_fact_extract_tests();
    run_personal_model_tests();
    run_personal_model_llm_extract_tests();
    run_personal_model_atomic_save_tests();
    run_personal_model_per_contact_tests();
#ifdef HU_ENABLE_SQLITE
    run_cross_channel_acl_tests();
    run_cross_channel_pipeline_tests();
    run_reflection_schema_tests();
#endif
    run_reflection_storage_tests();                 /* T2: stubbed-out when SQLite off */
    run_reflection_prompt_tests();                  /* T4: stubbed-out when SQLite off */
    run_reflection_orchestration_tests();           /* T5: stubbed-out when SQLite off */
    run_reflection_consumer_tests();                /* T6: stubbed-out when SQLite off */
    run_reflection_turn_source_tests();             /* T9fu: stubbed-out when SQLite off */
    run_reflection_quorum_tests();                  /* T11: stubbed-out when SQLite off */
    run_personal_model_reflection_slice_tests();    /* T7: stubbed-out when SQLite off */
    run_reflection_retire_on_contradiction_tests(); /* T8: stubbed-out when SQLite off */
    run_reflection_e2e_tests();                     /* T10: stubbed-out when SQLite off */
    run_doctor_reflection_loop_tests();             /* T12: stubbed-out when SQLite off */
    run_emotional_context_tests();
    run_autoresponder_tests();
    run_autoresponder_eval_tests();
    run_contact_narrative_tests();
    run_causal_attribution_tests();
    run_identity_continuity_tests();
    run_audio_emotion_tests();
    run_style_adapter_tests();
    run_lora_export_tests();
    run_lora_nightly_tests();
    run_adapter_swap_tests();
    run_lora_subprocess_tests();
    run_style_critique_patterns_tests();
    run_style_self_critique_tests();
    run_personal_model_simulation_tests();
#ifdef HU_ENABLE_RL_FULL
    run_personal_model_fidelity_v2_tests();
    run_grpo_loss_tests();
    run_grpo_huml_tests();
    run_grpo_mlx_tests();
    run_grpo_e2e_tests();
#endif
#ifdef HU_HAS_LIBSODIUM
    run_persona_encryption_tests();
#endif
    run_persona_directive_channels_tests();
    run_persona_overlay_render_tests();
#if defined(HU_HAS_IMESSAGE) && defined(HU_HAS_TELEGRAM)
    run_channel_overlay_apply_tests();
#endif
    run_filler_recency_tests();
    run_contact_send_recency_tests();
#ifdef HU_ENABLE_ML
    run_dpo_miner_tests();
#endif
    run_sprint3_hybrid_recall_tests();
    /* PR #115: removed 3 ghost-test calls (see declaration block above):
     * run_config_identity_links_tests, run_memory_session_scoping_tests,
     * run_imessage_outbound_dedup_tests. */
    run_filler_pctt_tests();
    run_hallucination_guard_tests();
    run_humor_fw_tests();
    run_self_improve_tests();
    run_sycophancy_guard_tests();
    run_trust_calibration_tests();
    run_vision_ocr_tests();
    run_markdown_loader_tests();
    run_structured_output_tests();
    run_anticipatory_state_tests();
    run_canvas_tool_tests();
    run_canvas_e2e_tests();
    run_canvas_persist_tests();
    run_canvas_render_tests();
    run_homebrew_formula_tests();
#ifdef HU_ENABLE_RL_FULL
    /* Phase 5 Task 2 (RL SOTA): bootstrap CI suite — only linked when
     * the RL-full gate is ON, so default release/dev builds are byte-
     * identical to the pre-Phase-5 test surface. */
    run_bootstrap_ci_tests();
    /* Phase 5 Task 3 (RL SOTA): external-LLM judge vtable + canned. */
    run_eval_judge_external_tests();
    run_leaderboard_tests();
    run_eval_gate_tests();
    run_stock_baseline_tests();
    run_apple_fm_client_tests();
    run_gemini_nano_client_tests();
    run_competitive_harness_tests();
    run_cli_eval_phase5_tests();
    run_cli_demo_evidence_tests();
    run_lora_ab_require_positive_tests();
    run_runner_eval_gate_tests();
    run_daemon_reaction_poll_tests();
    run_proof_directory_tests();
    run_e2e_closed_loop_tests();
#endif

    HU_TEST_REPORT();
    HU_TEST_EXIT();
}
