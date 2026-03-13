---
title: Concept-to-File Index
description: Maps concepts and subsystems to their primary source and test files
---

# Concept-to-File Index

Quick reference mapping concepts to their primary source and test files.
Use this to find the right files for a given task without searching the full codebase.

## Major Subsystems

| Concept                       | Primary Source Files                                                                                                           | Test Files                                      |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------- |
| **Allocator / arena / error** | `src/core/allocator.c`, `arena.c`, `error.c`                                                                                   | `test_allocator.c`, `test_core_extended.c`      |
| **JSON parsing / building**   | `src/core/json.c`, `src/json_util.c`                                                                                           | `test_json.c`, `test_json_extended.c`           |
| **String / slice**            | `src/core/string.c`, `src/core/slice.c`                                                                                        | `test_string.c`, `test_slice.c`                 |
| **HTTP client**               | `src/core/http.c`, `src/http_util.c`                                                                                           | `test_http.c`                                   |
| **Config parsing**            | `src/config_parse.c`, `config_parse_agent.c`, `config_parse_channels.c`, `config_parse_providers.c`, `config_parse_behavior.c` | `test_config_parse.c`, `test_config_extended.c` |
| **Config validation**         | `src/config_validate.c`, `src/config_schema.c`                                                                                 | `test_config_validation.c`                      |
| **Config merge / migrate**    | `src/config_merge.c`, `src/config_migrate.c`                                                                                   | `test_config_migrate.c`                         |
| **Config getters**            | `src/config_getters.c`, `src/config_serialize.c`                                                                               | `test_config_getters.c`                         |

## Agent

| Concept                           | Primary Source Files                                           | Test Files                                                                   |
| --------------------------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| **Agent loop / turn**             | `src/agent/agent.c`, `agent_turn.c`, `agent_stream.c`          | `test_agent_modules.c`, `test_agent_subsystems.c`, `test_e2e_conversation.c` |
| **Planner / dispatcher**          | `src/agent/planner.c`, `dispatcher.c`                          | `test_agent_extended.c`                                                      |
| **DAG / LLMCompiler**             | `src/agent/dag.c`, `dag_executor.c`, `llm_compiler.c`          | `test_dag.c`                                                                 |
| **Tool router**                   | `src/agent/tool_router.c`                                      | `test_tool_router.c`                                                         |
| **Prompt building**               | `src/agent/prompt.c`, `context_tokens.c`, `memory_loader.c`    | `test_prompt.c`                                                              |
| **Proactive / governor**          | `src/agent/proactive.c`, `governor.c`, `arbitrator.c`          | `test_proactive.c`, `test_governor.c`, `test_arbitrator.c`                   |
| **Superhuman / commitment**       | `src/agent/superhuman.c`, `commitment.c`, `commitment_store.c` | `test_superhuman.c`, `test_commitment.c`                                     |
| **Theory of mind / anticipatory** | `src/agent/theory_of_mind.c`, `anticipatory.c`, `awareness.c`  | `test_theory_of_mind.c`, `test_anticipatory.c`, `test_awareness.c`           |
| **Teams / collaboration**         | `src/agent/team.c`, `collab_planning.c`, `conv_goals.c`        | `test_agent_teams.c`, `test_collab_planning.c`, `test_conv_goals.c`          |
| **Input guard**                   | `src/agent/input_guard.c`                                      | `test_input_guard.c`                                                         |
| **Reflection**                    | `src/agent/reflection.c`                                       | `test_reflection.c`, `test_reflection_advanced.c`                            |

## Providers

| Concept                         | Primary Source Files                                | Test Files                               |
| ------------------------------- | --------------------------------------------------- | ---------------------------------------- |
| **Provider factory / router**   | `src/providers/factory.c`, `router.c`               | `test_provider.c`, `test_provider_all.c` |
| **OpenAI / Anthropic / Gemini** | `src/providers/openai.c`, `anthropic.c`, `gemini.c` | `test_provider_all.c`                    |
| **Ollama / local**              | `src/providers/ollama.c`                            | `test_ollama_integration.c`              |
| **SSE streaming**               | `src/providers/sse.c`, `src/sse/sse_client.c`       | `test_sse.c`, `test_streaming.c`         |

## Channels

| Concept                        | Primary Source Files                                           | Test Files                                                        |
| ------------------------------ | -------------------------------------------------------------- | ----------------------------------------------------------------- |
| **Channel factory / catalog**  | `src/channel_catalog.c`, `channel_manager.c`, `channel_loop.c` | `test_channel.c`, `test_channel_manager.c`, `test_channel_loop.c` |
| **CLI**                        | `src/channels/cli.c`, `src/agent/cli.c`                        | `test_cli.c`                                                      |
| **Telegram / Discord / Slack** | `src/channels/telegram.c`, `discord.c`, `slack.c`              | `test_channel_all.c`                                              |
| **iMessage**                   | `src/channels/imessage.c`                                      | `test_imessage_extended.c`                                        |
| **Gmail**                      | `src/channels/gmail.c`                                         | `test_gmail.c`                                                    |
| **Voice**                      | `src/channels/voice_channel.c`, `voice_realtime.c`             | `test_voice.c`, `test_voice_integration.c`                        |

## Tools

| Concept                 | Primary Source Files                                                 | Test Files                                |
| ----------------------- | -------------------------------------------------------------------- | ----------------------------------------- |
| **Tool factory**        | `src/tools/factory.c`                                                | `test_tool.c`, `test_tools_all.c`         |
| **Shell / file tools**  | `src/tools/shell.c`, `file_read.c`, `file_write.c`                   | `test_tools_all.c` (Shell/File suite)     |
| **Web / network tools** | `src/tools/http_request.c`, `web_fetch.c`                            | `test_tools_all.c` (Web/Network suite)    |
| **Memory tools**        | `src/tools/memory_recall.c`, `memory_store.c`                        | `test_tools_all.c` (Memory/Message suite) |
| **Cron tools**          | `src/tools/cron_add.c`, `cron_remove.c`, `cron_list.c`, `cron_run.c` | `test_cron.c`                             |

## Memory

| Concept                         | Primary Source Files                                                                                           | Test Files                                                                       |
| ------------------------------- | -------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| **Memory factory / engines**    | `src/memory/factory.c`, `engines/registry.c`, `engines/sqlite.c`, `engines/markdown.c`, `engines/memory_lru.c` | `test_memory.c`, `test_memory_full.c`, `test_memory_engines_ext.c`               |
| **Retrieval / hybrid search**   | `src/memory/retrieval/engine.c`, `hybrid.c`, `keyword.c`, `reranker.c`                                         | `test_retrieval.c`                                                               |
| **Vector / embeddings**         | `src/memory/vector/embeddings.c`, `store.c`, `chunker.c`                                                       | `test_vector.c`, `test_vector_full.c`, `test_vector_stores.c`                    |
| **QMD (query memory dispatch)** | `src/memory/retrieval/qmd.c`                                                                                   | `test_qmd.c`                                                                     |
| **Consolidation**               | `src/memory/consolidation.c`, `consolidation_engine.c`                                                         | `test_consolidation.c`, `test_consolidation_engine.c`                            |
| **RAG pipeline**                | `src/rag.c`, `src/memory/rag_pipeline.c`                                                                       | `test_rag.c`, `test_rag_pipeline.c`                                              |
| **Episodic / STM**              | `src/memory/episodic.c`, `stm.c`                                                                               | `test_episodic.c`, `test_stm.c`                                                  |
| **Emotional graph**             | `src/memory/emotional_graph.c`, `emotional_residue.c`, `emotional_moments.c`                                   | `test_emotional_graph.c`, `test_emotional_residue.c`, `test_emotional_moments.c` |
| **Forgetting / degradation**    | `src/memory/forgetting.c`, `forgetting_curve.c`, `degradation.c`                                               | `test_forgetting_curve.c`, `test_degradation.c`, `test_memory_degradation.c`     |
| **Lifecycle / hygiene**         | `src/memory/lifecycle/cache.c`, `hygiene.c`, `summarizer.c`                                                    | `test_lifecycle.c`                                                               |
| **Deep memory / extract**       | `src/memory/deep_memory.c`, `deep_extract.c`                                                                   | `test_deep_memory.c`, `test_deep_extract.c`                                      |
| **Cognitive**                   | `src/memory/cognitive.c`                                                                                       | `test_cognitive.c`                                                               |
| **Graph (knowledge)**           | `src/memory/graph.c`, `fast_capture.c`                                                                         | `test_graph.c`, `test_fast_capture.c`                                            |
| **Connections / promotion**     | `src/memory/connections.c`, `promotion.c`                                                                      | `test_promotion.c`                                                               |

## Security

| Concept             | Primary Source Files                                                 | Test Files                                                                |
| ------------------- | -------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| **Vault / secrets** | `src/security/vault.c`, `secrets.c`                                  | `test_vault.c`, `test_crypto.c`                                           |
| **Pairing**         | `src/security/pairing.c`                                             | `test_pairing.c`                                                          |
| **Policy engine**   | `src/security/policy.c`, `policy_engine.c`                           | `test_security.c`, `test_security_extended.c`, `test_security_pipeline.c` |
| **Sandbox**         | `src/security/sandbox.c`, `landlock.c`, `firejail.c`, `bubblewrap.c` | `test_security.c`                                                         |
| **Adversarial**     | `src/security/adversarial.c`                                         | `test_adversarial.c`, `test_adversarial_detect.c`                         |
| **Auth**            | `src/auth.c`                                                         | `test_auth.c`                                                             |
| **Path security**   | `src/net_security.c`                                                 | `test_net_security.c`, `test_path_security.c`                             |
| **Privacy audit**   | `src/security/audit.c`                                               | `test_privacy_audit.c`                                                    |

## Gateway

| Concept              | Primary Source Files                                                                      | Test Files                              |
| -------------------- | ----------------------------------------------------------------------------------------- | --------------------------------------- |
| **HTTP gateway**     | `src/gateway/gateway.c`                                                                   | `test_gateway.c`, `test_gateway_http.c` |
| **Control protocol** | `src/gateway/control_protocol.c`, `cp_chat.c`, `cp_config.c`, `cp_admin.c`, `cp_memory.c` | `test_gateway_extended.c`               |
| **WebSocket server** | `src/gateway/ws_server.c`                                                                 | `test_gateway_extended.c`               |
| **OAuth**            | `src/gateway/oauth.c`                                                                     | `test_oauth.c`, `test_gateway_auth.c`   |
| **OpenAI compat**    | `src/gateway/openai_compat.c`                                                             | `test_gateway_extended.c`               |
| **Event bridge**     | `src/gateway/event_bridge.c`                                                              | `test_gateway_extended.c`               |
| **Tenancy**          | `src/gateway/tenant.c`                                                                    | `test_tenant.c`                         |

## Persona

| Concept                         | Primary Source Files                   | Test Files                                   |
| ------------------------------- | -------------------------------------- | -------------------------------------------- |
| **Persona loading / parsing**   | `src/persona/persona.c`                | `test_persona.c`                             |
| **Persona creation / analysis** | `src/persona/creator.c`, `analyzer.c`  | `test_persona.c`                             |
| **Example selection**           | `src/persona/examples.c`, `sampler.c`  | `test_persona.c`                             |
| **Persona mood / circadian**    | `src/persona/mood.c`, `circadian.c`    | `test_persona_mood.c`, `test_circadian.c`    |
| **Relationship tracking**       | `src/persona/relationship.c`           | `test_relationship.c`                        |
| **Style cloning**               | `src/persona/style_clone.c`            | `test_style_clone.c`                         |
| **Training / feedback**         | `src/persona/training.c`, `feedback.c` | `test_persona_training.c`, `test_feedback.c` |

## Context

| Concept            | Primary Source Files           | Test Files              |
| ------------------ | ------------------------------ | ----------------------- |
| **Conversation**   | `src/context/conversation.c`   | `test_conversation.c`   |
| **Mood**           | `src/context/mood.c`           | `test_mood.c`           |
| **Social graph**   | `src/context/social_graph.c`   | `test_social_graph.c`   |
| **Vision**         | `src/context/vision.c`         | `test_vision.c`         |
| **Style tracker**  | `src/context/style_tracker.c`  | `test_style_tracker.c`  |
| **Self-awareness** | `src/context/self_awareness.c` | `test_self_awareness.c` |

## Other Subsystems

| Concept                          | Primary Source Files                                                      | Test Files                                                    |
| -------------------------------- | ------------------------------------------------------------------------- | ------------------------------------------------------------- |
| **Runtime (native/docker/wasm)** | `src/runtime/native.c`, `docker.c`, `wasm_rt.c`, `factory.c`              | `test_runtime.c`, `test_runtime_bundle.c`                     |
| **Feeds (social/google/apple)**  | `src/feeds/processor.c`, `social.c`, `google.c`, `apple.c`                | `test_feeds.c`, `test_feed_processor.c`                       |
| **Intelligence / skills**        | `src/intelligence/skills.c`, `skill_system.c`, `reflection.c`             | `test_skills.c`, `test_skill_system.c`, `test_intelligence.c` |
| **Peripherals**                  | `src/peripherals/factory.c`, `arduino.c`, `stm32.c`, `rpi.c`              | `test_peripheral.c`                                           |
| **Observability**                | `src/observability/log_observer.c`, `metrics_observer.c`, `bth_metrics.c` | `test_observer.c`, `test_bth_metrics.c`                       |
| **Subagent / MCP**               | `src/subagent.c`, `src/mcp.c`, `src/mcp_server.c`                         | `test_subagent.c`, `test_mcp.c`                               |
| **Session**                      | `src/session.c`                                                           | `test_session.c`                                              |
| **WebSocket client**             | `src/websocket/websocket.c`                                               | `test_websocket.c`                                            |
| **Tunnel**                       | `src/tunnel/ngrok.c`, `cloudflare.c`, `tailscale.c`                       | `test_tunnel.c`                                               |
| **Voice / TTS**                  | `src/voice.c`, `src/tts/audio_pipeline.c`, `emotion_map.c`                | `test_voice.c`, `test_audio_pipeline.c`, `test_emotion_map.c` |
| **Paperclip**                    | `src/paperclip/client.c`, `heartbeat.c`                                   | `test_paperclip.c`                                            |

## AGI Frontiers / Intelligence

| Concept                     | Primary Source Files                             | Test Files               |
| --------------------------- | ------------------------------------------------ | ------------------------ |
| **Constitutional AI**       | `src/agent/constitutional.c`                     | `test_agent_modules.c`   |
| **Goal autonomy**           | `src/agent/goals.c`                              | `test_goal_engine.c`     |
| **Agent orchestrator**      | `src/agent/orchestrator.c`, `orchestrator_llm.c` | `test_agi_frontiers.c`   |
| **Agent registry**          | `src/agent/registry.c`                           | `test_agent_registry.c`  |
| **Speculative execution**   | `src/agent/speculative.c`                        | `test_agi_frontiers.c`   |
| **Tree-of-thought**         | `src/agent/tree_of_thought.c`                    | `test_agent_modules.c`   |
| **Uncertainty estimation**  | `src/agent/uncertainty.c`                        | `test_agi_frontiers.c`   |
| **Online learning**         | `src/intelligence/online_learning.c`             | `test_agi_frontiers.c`   |
| **Self-improvement**        | `src/intelligence/self_improve.c`                | `test_agi_frontiers.c`   |
| **Value learning**          | `src/intelligence/value_learning.c`              | `test_value_learning.c`  |
| **World model**             | `src/intelligence/world_model.c`                 | `test_agi_frontiers.c`   |
| **Strategy learner**        | `src/memory/retrieval/strategy_learner.c`        | `test_agi_frontiers.c`   |
| **Peripheral control tool** | `src/tools/peripheral_ctrl.c`                    | `test_peripheral_ctrl.c` |
| **Skill runner tool**       | `src/tools/skill_run.c`                          | `test_agent_registry.c`  |
| **PWA bridge**              | `src/pwa/bridge.c`, `drivers.c`                  | `test_pwa.c`             |
| **ML subsystem**            | `src/ml/gpt.c`, `train.c`, `prepare.c`           | `test_ml.c`              |
| **Research feeds**          | `src/feeds/research.c`, `file_ingest.c`          | `test_research_feeds.c`  |
