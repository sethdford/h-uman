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
| **Config schema generation**  | `src/config_schema.c`, `include/human/config_schema.h`                                         | `test_config_schema.c`, `test_config_validation.c` |

## Agent

| Concept                           | Primary Source Files                                           | Test Files                                                                   |
| --------------------------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| **Agent loop / turn**             | `src/agent/agent.c`, `agent_turn.c`, `agent_stream.c`          | `test_agent_modules.c`, `test_agent_subsystems.c`, `test_e2e_conversation.c` |
| **Planner / dispatcher**          | `src/agent/planner.c`, `dispatcher.c`                          | `test_agent_extended.c`                                                      |
| **DAG / LLMCompiler**             | `src/agent/dag.c`, `dag_executor.c`, `llm_compiler.c`          | `test_dag.c`                                                                 |
| **HuLa (program IR / emergence)** | `src/agent/hula.c`, `hula_compiler.c`, `hula_emergence.c`, `agent_turn.c` (integration); guide: [`docs/guides/hula.md`](guides/hula.md) | `test_hula.c`                                                                |
| **Metacognition (loop / policy)** | `src/cognition/metacognition.c`, `src/agent/agent_turn.c`, `src/agent/spawn.c` (`hu_spawn_config_apply_current_tool_agent`) | `test_metacognition.c`, `test_config_extended.c`; ops: `docs/operations/metacog-hula-production.md` |
| **Tool router**                   | `src/agent/tool_router.c`                                      | `test_tool_router.c`                                                         |
| **Prompt building**               | `src/agent/prompt.c`, `context_tokens.c`, `memory_loader.c`    | `test_prompt.c`                                                              |
| **Proactive / governor**          | `src/agent/proactive.c`, `governor.c`, `arbitrator.c`          | `test_proactive.c`, `test_governor.c`, `test_arbitrator.c`                   |
| **Superhuman / commitment**       | `src/agent/superhuman.c`, `commitment.c`, `commitment_store.c` | `test_superhuman.c`, `test_commitment.c`                                     |
| **Theory of mind / anticipatory** | `src/agent/theory_of_mind.c`, `anticipatory.c`, `awareness.c`  | `test_theory_of_mind.c`, `test_anticipatory.c`, `test_awareness.c`           |
| **Teams / collaboration**         | `src/agent/team.c`, `collab_planning.c`, `conv_goals.c`        | `test_agent_teams.c`, `test_collab_planning.c`, `test_conv_goals.c`          |
| **Input guard**                   | `src/agent/input_guard.c`                                      | `test_input_guard.c`                                                         |
| **Reflection**                    | `src/agent/reflection.c`                                       | `test_reflection.c`, `test_reflection_advanced.c`                            |
| **Prompt cache**                  | `src/agent/prompt_cache.c`, `include/human/agent/prompt_cache.h` | `test_sota_wiring.c`                                                      |
| **Agent Communication Protocol (ACP)** | `src/agent/agent_comm.c`, `include/human/agent/agent_comm.h` | `test_agent_communication.c`, `test_sota_wiring.c`                         |
| **ACP bridge**                    | `src/agent/acp_bridge.c`, `include/human/agent/acp_bridge.h`   | `—`                                                                          |
| **KV cache**                      | `src/agent/kv_cache.c`, `include/human/agent/kv_cache.h`       | `—`                                                                          |
| **HuLa compiler (auto-verify)** | `src/agent/hula_compiler.c`, `include/human/agent/hula_compiler.h` | `test_hula.c`, `test_sota_wiring.c`                                      |

## Providers

| Concept                         | Primary Source Files                                | Test Files                               |
| ------------------------------- | --------------------------------------------------- | ---------------------------------------- |
| **Provider factory / router**   | `src/providers/factory.c`, `router.c`               | `test_provider.c`, `test_provider_all.c` |
| **OpenAI / Anthropic / Gemini** | `src/providers/openai.c`, `anthropic.c`, `gemini.c` | `test_provider_all.c`                    |
| **Ollama / local**              | `src/providers/ollama.c`                            | `test_ollama_integration.c`              |
| **CoreML / MLX (on-device)**    | `src/providers/coreml.c`, `include/human/providers/coreml.h` | `test_coreml_provider.c`            |
| **SSE streaming**               | `src/providers/sse.c`, `src/sse/sse_client.c`       | `test_sse.c`, `test_streaming.c`         |
| **HUML checkpoint (on-device)** | `src/providers/huml.c`, `include/human/providers/huml.h` | `test_ml.c`                          |
| **Embedded / llama-cli**        | `src/providers/embedded.c`, `include/human/providers/embedded.h` | `test_ml.c`                    |
| **Apple Intelligence (on-device)** | `src/providers/apple.c`, `include/human/providers/apple.h`, `apps/shared/HumanKit/Sources/HumanOnDeviceServer/` | `test_apple_provider.c`, `OnDeviceServerTests.swift` |

### Voice

| Concept | Primary Source Files | Test Files |
| --- | --- | --- |
| **Voice provider vtable** | `include/human/voice/provider.h` | `test_voice_provider.c` |
| **OpenAI Realtime provider** | `src/voice/realtime.c` | `test_voice_rt_openai.c`, `test_voice_provider.c` |
| **Gemini Live provider** | `src/voice/gemini_live.c` | `test_gemini_live.c`, `test_voice_provider.c` |
| **Voice session API** | `src/voice/session.c`, `include/human/voice/session.h` | `test_voice_session.c` |
| **Duplex turn-taking** | `src/voice/duplex.c`, `src/voice/semantic_eot.c` | `test_voice_duplex.c` |
| **Gateway voice streaming** | `src/gateway/cp_voice_stream.c` | `test_gateway_voice.c` |

## Channels

| Concept                        | Primary Source Files                                           | Test Files                                                        |
| ------------------------------ | -------------------------------------------------------------- | ----------------------------------------------------------------- |
| **Channel factory / catalog**  | `src/channel_catalog.c`, `channel_manager.c`, `channel_loop.c` | `test_channel.c`, `test_channel_manager.c`, `test_channel_loop.c` |
| **CLI**                        | `src/channels/cli.c`, `src/agent/cli.c`                        | `test_cli.c`                                                      |
| **Telegram / Discord / Slack** | `src/channels/telegram.c`, `discord.c`, `slack.c`              | `test_channel_all.c`                                              |
| **iMessage**                   | `src/channels/imessage.c`                                      | `test_imessage_extended.c`                                        |
| **Gmail**                      | `src/channels/gmail.c`                                         | `test_gmail.c`                                                    |
| **Voice**                      | `src/channels/voice_channel.c`, `src/voice.c`                  | `test_voice.c`, `test_cartesia.c`, `test_local_voice.c`           |

## Voice

| Concept                              | Primary Source Files                                                         | Test Files                                           |
| ------------------------------------ | ---------------------------------------------------------------------------- | ---------------------------------------------------- |
| **Semantic EOT (Phoenix-VAD)**     | `src/voice/semantic_eot.c`, `include/human/voice/semantic_eot.h`             | `test_sota_research.c`, `test_sota_wiring.c`         |
| **Emotion–voice map**                | `src/voice/emotion_voice_map.c`, `include/human/voice/emotion_voice_map.h`   | `test_sota_wiring.c`                                 |
| **Audio emotion (STT features)**   | `src/voice/audio_emotion.c`, `include/human/voice/audio_emotion.h`           | `—`                                                  |
| **Turn signal (LLM turn-taking)**  | `src/voice/turn_signal.c`, `include/human/voice/turn_signal.h`               | `test_turn_signal.c`                                 |
| **Voice cloning (Cartesia)**       | `src/tts/voice_clone.c`, `include/human/tts/voice_clone.h`                  | `test_voice_clone.c`                                 |
| **Voice clone gateway**            | `src/gateway/cp_voice_clone.c`                                               | `test_gateway_voice.c`                               |
| **Voice clone tool**               | `src/tools/voice_clone.c`, `include/human/tools/voice_clone.h`              | `test_voice_clone.c`                                 |
| **Send voice message tool**        | `src/tools/send_voice_message.c`, `include/human/tools/send_voice_message.h`| `test_send_voice_message.c`                          |
| **Voice clone UI**                 | `ui/src/components/hu-voice-clone.ts`                                        | `—`                                                  |

## Tools

| Concept                 | Primary Source Files                                                 | Test Files                                |
| ----------------------- | -------------------------------------------------------------------- | ----------------------------------------- |
| **Tool factory**        | `src/tools/factory.c`                                                | `test_tool.c`, `test_tools_all.c`         |
| **Tool thread-safety flags** | `include/human/tool.h` (`hu_tool_flags_t`, `HU_TOOL_FLAG_THREAD_SAFE`) | `—`                                  |
| **Shell / file tools**  | `src/tools/shell.c`, `file_read.c`, `file_write.c`                   | `test_tools_all.c` (Shell/File suite)     |
| **Web / network tools** | `src/tools/http_request.c`, `web_fetch.c`                            | `test_tools_all.c` (Web/Network suite)    |
| **Memory tools**        | `src/tools/memory_recall.c`, `memory_store.c`                        | `test_tools_all.c` (Memory/Message suite) |
| **Cron tools**          | `src/tools/cron_add.c`, `cron_remove.c`, `cron_list.c`, `cron_run.c` | `test_cron.c`                             |
| **Computer use / LSP**  | `src/tools/computer_use.c`, `lsp.c`                                  | `test_computer_use.c`, `test_lsp.c`       |
| **Tool result cache (TTL)** | `src/tools/cache_ttl.c`, `include/human/tools/cache_ttl.h`         | `test_sota_wiring.c`                      |
| **Live Canvas / A2UI**  | `src/tools/canvas.c`, `include/human/tools/canvas.h`, `src/gateway/cp_canvas.c` | `test_canvas_tool.c`, `test_canvas_e2e.c` |
| **Canvas UI**           | `ui/src/components/hu-canvas.ts`, `hu-canvas-sandbox.ts`, `hu-canvas-editor.ts`, `ui/src/canvas-harness.ts`, `ui/src/views/canvas-view.ts` | — |

## Memory

| Concept                         | Primary Source Files                                                                                           | Test Files                                                                       |
| ------------------------------- | -------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| **Memory factory / engines**    | `src/memory/factory.c`, `engines/registry.c`, `engines/sqlite.c`, `engines/markdown.c`, `engines/memory_lru.c` | `test_memory.c`, `test_memory_full.c`, `test_memory_engines_ext.c`               |
| **SQL transaction helper**      | `src/memory/sql_transaction.c`, `include/human/memory/sql_transaction.h`                                       | `test_sql_transaction.c`                                                         |
| **Retrieval / hybrid search**   | `src/memory/retrieval/engine.c`, `hybrid.c`, `keyword.c`, `reranker.c`                                         | `test_retrieval.c`                                                               |
| **Vector / embeddings**         | `src/memory/vector/embeddings.c`, `store.c`, `chunker.c`                                                       | `test_vector.c`, `test_vector_full.c`, `test_vector_stores.c`                    |
| **QMD (query memory dispatch)** | `src/memory/retrieval/qmd.c`                                                                                   | `test_qmd.c`                                                                     |
| **Consolidation**               | `src/memory/consolidation.c`, `consolidation_engine.c`                                                         | `test_consolidation.c`, `test_consolidation_engine.c`                            |
| **RAG pipeline**                | `src/rag.c`, `src/memory/rag_pipeline.c`                                                                       | `test_rag.c`, `test_rag_pipeline.c`                                              |
| **Relational episodes**         | `src/memory/relational_episode.c`, `include/human/memory/relational_episode.h`                                 | `test_humanness_frontiers.c`                                                     |
| **Episodic / STM**              | `src/memory/episodic.c`, `stm.c`                                                                               | `test_episodic.c`, `test_stm.c`                                                  |
| **Emotional graph**             | `src/memory/emotional_graph.c`, `emotional_residue.c`, `emotional_moments.c`                                   | `test_emotional_graph.c`, `test_emotional_residue.c`, `test_emotional_moments.c` |
| **Forgetting / degradation**    | `src/memory/forgetting.c`, `forgetting_curve.c`, `degradation.c`                                               | `test_forgetting_curve.c`, `test_degradation.c`, `test_memory_degradation.c`     |
| **Lifecycle / hygiene**         | `src/memory/lifecycle/cache.c`, `hygiene.c`, `summarizer.c`                                                    | `test_lifecycle.c`                                                               |
| **Deep memory / extract**       | `src/memory/deep_memory.c`, `deep_extract.c`                                                                   | `test_deep_memory.c`, `test_deep_extract.c`                                      |
| **Cognitive**                   | `src/memory/cognitive.c`                                                                                       | `test_cognitive.c`                                                               |
| **Graph (knowledge)**           | `src/memory/graph.c`, `fast_capture.c`                                                                         | `test_graph.c`, `test_fast_capture.c`                                            |
| **Connections / promotion**     | `src/memory/connections.c`, `promotion.c`                                                                      | `test_promotion.c`                                                               |
| **Graph index (MAGMA, spreading activation)** | `src/memory/graph_index.c`, `include/human/memory/graph_index.h`                               | `test_sota_research.c`, `test_sota_wiring.c`                                     |
| **Entropy gate**                | `src/memory/retrieval/entropy_gate.c`, `include/human/memory/entropy_gate.h`                                   | `test_entropy_gate.c`, `test_sota_wiring.c`                                      |

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
| **Arg inspector (AEGIS-style)** | `src/security/arg_inspector.c`, `include/human/security/arg_inspector.h`                       | `test_sota_wiring.c`                                                      |
| **CoT audit**       | `src/security/cot_audit.c`, `include/human/security/cot_audit.h`     | `test_cot_audit.c`, `test_sota_wiring.c`                                  |
| **Confusable normalization** | `src/security/normalize.c`, `include/human/security/normalize.h` | `test_normalize.c`                                                       |
| **Content moderation** | `src/security/moderation.c` (word-boundary matching via `mod_contains_word`; `hu_normalize_confusables` for confusable-safe checks) | `test_moderation.c`, `test_companion_safety.c`                           |

## Gateway

| Concept              | Primary Source Files                                                                      | Test Files                              |
| -------------------- | ----------------------------------------------------------------------------------------- | --------------------------------------- |
| **HTTP gateway**     | `src/gateway/gateway.c`                                                                   | `test_gateway.c`, `test_gateway_http.c` |
| **Control protocol** | `src/gateway/control_protocol.c`, `cp_chat.c`, `cp_config.c`, `cp_admin.c` (incl. `hula.traces.*`), `cp_voice.c`, `cp_memory.c` | `test_gateway_extended.c` |
| **WebSocket server** | `src/gateway/ws_server.c`                                                                 | `test_gateway_extended.c`               |
| **OAuth**            | `src/gateway/oauth.c`                                                                     | `test_oauth.c`, `test_gateway_auth.c`   |
| **OpenAI compat**    | `src/gateway/openai_compat.c`                                                             | `test_gateway_extended.c`               |
| **Event bridge**     | `src/gateway/event_bridge.c`                                                              | `test_gateway_extended.c`               |
| **Streaming voice**  | `src/gateway/cp_voice_stream.c`, `src/tts/cartesia_stream.c`, `docs/streaming-voice.md`   | `test_cartesia_stream.c`, `test_gateway_voice.c` |
| **Tenancy**          | `src/gateway/tenant.c`                                                                    | `test_tenant.c`                         |
| **Control protocol: security (CoT audit)** | `src/gateway/cp_security.c`                                                     | `test_gateway_extended.c`               |
| **Control protocol: Turing score** | `src/gateway/cp_turing.c`                                                         | `test_gateway_extended.c`               |

## MCP

| Concept            | Primary Source Files                                      | Test Files           |
| ------------------ | --------------------------------------------------------- | -------------------- |
| **MCP context (timeout, SERF)** | `src/gateway/mcp_context.c`, `include/human/mcp_context.h` | `test_sota_wiring.c` |

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
| **PersonaFuse (per-channel overlays)** | `src/persona/persona_fuse.c`, `include/human/persona/persona_fuse.h` | `—`                                 |

## Humanness Frontiers

| Concept                          | Primary Source Files                                         | Test Files                      |
| -------------------------------- | ------------------------------------------------------------ | ------------------------------- |
| **Somatic State Engine**         | `src/persona/somatic.c`, `include/human/persona/somatic.h`  | `test_humanness_frontiers.c`    |
| **Message Choreography**         | `src/agent/choreography.c`, `include/human/agent/choreography.h` | `test_humanness_frontiers.c` |
| **Narrative Self**               | `src/persona/narrative_self.c`, `include/human/persona/narrative_self.h` | `test_humanness_frontiers.c` |
| **Novelty Detection**            | `src/cognition/novelty.c`, `include/human/cognition/novelty.h` | `test_humanness_frontiers.c`  |
| **Attachment Dynamics**          | `src/cognition/attachment.c`, `include/human/cognition/attachment.h` | `test_humanness_frontiers.c` |
| **Rupture-Repair Cycle**         | `src/cognition/rupture_repair.c`, `include/human/cognition/rupture_repair.h` | `test_humanness_frontiers.c` |
| **Relational Episode Memory**    | `src/memory/relational_episode.c`, `include/human/memory/relational_episode.h` | `test_humanness_frontiers.c` |
| **Presence Gradient**            | `src/cognition/presence.c`, `include/human/cognition/presence.h` | `test_humanness_frontiers.c` |
| **Micro-Expression Text Layer**  | `src/persona/micro_expression.c`, `include/human/persona/micro_expression.h` | `test_humanness_frontiers.c` |
| **Creative Voice Engine**        | `src/persona/creative_voice.c`, `include/human/persona/creative_voice.h` | `test_humanness_frontiers.c` |
| **Frontier persistence**         | `src/agent/frontier_persist.c`, `include/human/agent/frontier_persist.h` | `test_humanness_frontiers.c` |
| **Growth Narrative**             | `src/agent/growth_narrative.c`, `include/human/agent/growth_narrative.h` | `test_humanness_frontiers.c` |
| **Genuine Boundaries**           | `src/persona/genuine_boundaries.c`, `include/human/persona/genuine_boundaries.h` | `test_humanness_frontiers.c` |

## Context

| Concept            | Primary Source Files           | Test Files              |
| ------------------ | ------------------------------ | ----------------------- |
| **Conversation**   | `src/context/conversation.c`   | `test_conversation.c`   |
| **Mood**           | `src/context/mood.c`           | `test_mood.c`           |
| **Social graph**   | `src/context/social_graph.c`   | `test_social_graph.c`   |
| **Vision**         | `src/context/vision.c`         | `test_vision.c`         |
| **Style tracker**  | `src/context/style_tracker.c`  | `test_style_tracker.c`  |
| **Self-awareness** | `src/context/self_awareness.c` | `test_self_awareness.c` |

## Eval

| Concept                         | Primary Source Files                                         | Test Files                               |
| ------------------------------- | ------------------------------------------------------------ | ---------------------------------------- |
| **Turing score (S2S, 18-dim)**  | `src/eval/turing_score.c`, `include/human/eval/turing_score.h` | `test_turing_score.c`, `test_sota_research.c` |

## Other Subsystems

| Concept                                                | Primary Source Files                                                                                                            | Test Files                                                                         |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| **Runtime (native/docker/wasm)**                       | `src/runtime/native.c`, `docker.c`, `wasm_rt.c`, `factory.c`                                                                    | `test_runtime.c`, `test_runtime_bundle.c`                                          |
| **Feeds (social/google/apple/gmail/imessage/twitter)** | `src/feeds/processor.c`, `social.c`, `google.c`, `apple.c`, `gmail.c`, `imessage.c`, `twitter.c`, `research.c`, `file_ingest.c` | `test_feeds.c`, `test_feed_processor.c`, `test_research_feeds.c`                   |
| **Feed content sandboxing**    | `src/feeds/research.c` (XML quarantine for feed content)                                                                                    | `test_research_feeds.c`                                                            |
| **Intelligence / skills**                              | `src/intelligence/skills.c`, `skill_system.c`, `reflection.c`, `experience.c`                                                   | `test_skills.c`, `test_skill_system.c`, `test_intelligence.c`, `test_experience.c` |
| **Peripherals**                                        | `src/peripherals/factory.c`, `arduino.c`, `stm32.c`, `rpi.c`                                                                    | `test_peripheral.c`                                                                |
| **Observability**                                      | `src/observability/log_observer.c`, `metrics_observer.c`, `bth_metrics.c`                                                       | `test_observer.c`, `test_bth_metrics.c`                                            |
| **Subagent / MCP**                                     | `src/subagent.c`, `src/mcp.c`, `src/mcp_server.c`                                                                               | `test_subagent.c`, `test_mcp.c`                                                    |
| **Session**                                            | `src/session.c`                                                                                                                 | `test_session.c`                                                                   |
| **WebSocket client**                                   | `src/websocket/websocket.c`                                                                                                     | `test_websocket.c`, `test_ws_integration.c`                                        |
| **Tunnel**                                             | `src/tunnel/ngrok.c`, `cloudflare.c`, `tailscale.c`                                                                             | `test_tunnel.c`                                                                    |
| **Voice / TTS / WebRTC**                               | `src/voice.c`, `src/voice_config.c`, `src/voice/realtime.c`, `src/voice/webrtc.c`, `src/tts/audio_pipeline.c`, `src/tts/emotion_map.c`                | `test_voice.c`, `test_webrtc.c`, `test_audio_pipeline.c`, `test_emotion_map.c`     |
| **Paperclip**                                          | `src/paperclip/client.c`, `heartbeat.c`                                                                                         | `test_paperclip.c`                                                                 |
| **Eval**                                               | `src/eval.c`, `eval_suites/*.json`, `scripts/adversarial-eval-harness.py`, `scripts/redteam-eval-fleet.sh`, `scripts/redteam-live.sh` | `test_eval.c`, `test_adversarial_detect.c`                                         |
| **Daemon / worker pool**                         | `src/daemon.c`, `src/gateway/gateway.c`, `scripts/daemon-*`, `.claude/daemon-config.json` | `test_daemon.c` (if exists) |
| **A2A**                                                | `src/a2a.c`                                                                                                                     | `test_a2a.c`                                                                       |

## Design system / tokens

### UI Dashboard

- Web dashboard shell / routing — `ui/src/app.ts`
- Theme CSS + utilities — `ui/src/styles/theme.css`
- Scroll-driven motion — `ui/src/styles/scroll-driven.css`
- Scroll entrance (Lit module + IO fallback) — `ui/src/styles/scroll-entrance.ts`, `ui/src/utils/scroll-entrance.ts`
- Card component (glass, tilt, mesh, entrance) — `ui/src/components/hu-card.ts`
- Input component (tonal variant) — `ui/src/components/hu-input.ts`
- Component catalog — `ui/catalog.html`
- View tests — `ui/src/views/views.test.ts`

## AGI Frontiers / Intelligence

| Concept                     | Primary Source Files                                                                                     | Test Files               |
| --------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------ |
| **Constitutional AI**       | `src/agent/constitutional.c`                                                                             | `test_agent_modules.c`   |
| **Goal autonomy**           | `src/agent/goals.c`                                                                                      | `test_goal_engine.c`     |
| **Agent orchestrator**      | `src/agent/orchestrator.c`, `orchestrator_llm.c`                                                         | `test_agi_frontiers.c`   |
| **Agent registry**          | `src/agent/registry.c`                                                                                   | `test_agent_registry.c`  |
| **Speculative execution**   | `src/agent/speculative.c`                                                                                | `test_agi_frontiers.c`   |
| **Tree-of-thought**         | `src/agent/tree_of_thought.c`                                                                            | `test_agent_modules.c`   |
| **Uncertainty estimation**  | `src/agent/uncertainty.c`                                                                                | `test_agi_frontiers.c`   |
| **Online learning**         | `src/intelligence/online_learning.c`                                                                     | `test_agi_frontiers.c`   |
| **Self-improvement**        | `src/intelligence/self_improve.c`                                                                        | `test_agi_frontiers.c`   |
| **Value learning**          | `src/intelligence/value_learning.c`                                                                      | `test_value_learning.c`  |
| **World model**             | `src/intelligence/world_model.c`                                                                         | `test_agi_frontiers.c`   |
| **Strategy learner**        | `src/memory/retrieval/strategy_learner.c`                                                                | `test_agi_frontiers.c`   |
| **Peripheral control tool** | `src/tools/peripheral_ctrl.c`                                                                            | `test_peripheral_ctrl.c` |
| **Skill runner tool**       | `src/tools/skill_run.c`                                                                                  | `test_agent_registry.c`  |
| **Skills ↔ agents**         | `docs/standards/ai/skills-vs-agents.md`, `docs/plans/2026-03-20-static-skills-dynamic-agents-unification.md` | `test_subsystems` (catalog) |
| **Skill prompt catalog**    | `src/skillforge.c` (`hu_skillforge_build_prompt_catalog`), `src/agent/agent_turn.c`                     | `test_subsystems`        |
| **PWA bridge**              | `src/pwa/bridge.c`, `context.c`, `drivers.c`, `learner.c`                                                | `test_pwa.c`             |
| **ML subsystem**            | `src/ml/gpt.c`, `train.c`, `prepare.c`, `tokenizer_bpe.c`, `dataloader.c`, `evaluator.c`, `experiment.c`, `dpo.c`, `lora.c`, `cli.c`, `checkpoint.c`, `agent_trainer.c`, `m3_frontier_adapter.c`, `include/human/ml/m3_frontier_adapter.h` | `test_ml.c` |
| **Research feeds**          | `src/feeds/research.c`, `file_ingest.c`, `gmail.c`, `imessage.c`, `twitter.c`                            | `test_research_feeds.c`  |
| **Fact extraction**         | `src/memory/fact_extract.c`, `include/human/memory/fact_extract.h`                                       | `test_fact_extract.c`    |
| **Hallucination guard**     | `src/memory/hallucination_guard.c`, `include/human/memory/hallucination_guard.h`                         | `test_hallucination_guard.c` |
| **Sycophancy guard**        | `src/security/sycophancy_guard.c`, `include/human/security/sycophancy_guard.h`                           | `test_sycophancy_guard.c` |
| **Trust calibration**       | `src/cognition/trust.c`, `include/human/cognition/trust.h`                                               | `test_trust_calibration.c` |
| **Consistency eval**        | `src/eval/consistency.c`, `include/human/eval/consistency.h`                                             | `test_consistency.c`     |
| **Humor framework**         | `src/persona/humor.c`, `include/human/persona/humor.h`                                                   | `test_humor_fw.c`        |
| **Markdown persona loader** | `src/persona/markdown_loader.c`, `include/human/persona/markdown_loader.h`                               | `test_markdown_loader.c` |
| **Self-improve (fidelity)** | `src/agent/self_improve.c`, `include/human/agent/self_improve.h`                                         | `test_self_improve.c`    |
| **Task store**              | `src/agent/task_store.c`, `src/gateway/cp_tasks.c`, `include/human/agent/task_store.h`                   | `test_task_store.c`      |
| **Vertex auth (ADC)**       | `src/core/vertex_auth.c`, `include/human/core/vertex_auth.h`                                             | `test_media_gen.c`       |
| **Vision OCR tool**         | `src/tools/vision_ocr.c`, `vision_ocr_apple.m`, `include/human/tools/vision_ocr.h`                      | `test_vision_ocr.c`      |
| **Media generation**        | `src/tools/media_image.c`, `media_video.c`, `media_gif.c`                                                | `test_media_gen.c`       |
| **Music (iTunes + Spotify + taste + persistence)** | `src/music.c`, `include/human/music.h`                                                            | `test_music.c`           |
| **Image gen (proactive + download)** | `src/tools/image_gen.c`, `include/human/tools/image_gen.h`                                            | `test_image_gen.c`       |
| **Send voice message**      | `src/tools/send_voice_message.c`, `include/human/tools/send_voice_message.h`                             | `test_send_voice_message.c` |

## Native client apps (`apps/`)

| Concept | Primary | Tests / CI |
| --- | --- | --- |
| **Shared Swift (HumanKit)** | `apps/shared/HumanKit/` | `swift test`; `HumaniOSUITests` depends on HumanChatUI |
| **iOS app** | `apps/ios/Sources/HumaniOS/` | `apps/ios/UITests/` (XCUITest); `.github/actions/ios-uitest`; `native-apps-fleet.yml` |
| **macOS app** | `apps/macos/Sources/HumanApp/` | `swift build` in CI / fleet |
| **Android app** | `apps/android/app/src/main/` | `app/src/androidTest/` (`connectedDebugAndroidTest` in fleet) |
| **Local runner** | — | `scripts/run-native-fleet-local.sh` |
| **Parity matrix (UI vs native vs gateway)** | `apps/shared/PARITY.md` | Reference for feature parity across surfaces |
