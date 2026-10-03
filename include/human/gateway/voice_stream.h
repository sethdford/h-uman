#ifndef HU_VOICE_STREAM_H
#define HU_VOICE_STREAM_H

#include "human/bus.h"
#include <stdbool.h>
#include <stdint.h>
#include "human/gateway/control_protocol.h"
#include "human/gateway/ws_server.h"

void hu_voice_stream_attach_bus(hu_bus_t *bus, hu_control_protocol_t *proto);
void hu_voice_stream_detach_bus(hu_bus_t *bus);

void hu_voice_stream_on_binary(hu_control_protocol_t *proto, hu_ws_conn_t *conn, const char *data,
                               size_t data_len);

void hu_voice_stream_on_conn_close(hu_ws_conn_t *conn);

/* What a reply chunk should be spoken as: the turn-signal token stripped, then
 * hu_speech_cleanup (texting forms such as lol/haha, emoji, *laughs*, URLs never
 * reach TTS; lmk -> "let me know"). Returns the length written to out (always
 * NUL-terminated when cap > 0); 0 means nothing in this chunk should be said. */
size_t hu_voice_stream_speakable(const char *chunk, size_t chunk_len, char *out, size_t cap);

/* Cartesia closes an idle TTS WebSocket; a session left quiet for several minutes then
 * produced a reply with no audio (measured: 0.00 s after ~6.7 min idle). The socket is
 * reopened before a spoken turn once it has been idle this long. */
#define HU_VOICE_STREAM_TTS_IDLE_REOPEN_MS (4 * 60 * 1000)

/* True when a TTS socket last used at last_used_ms should be reopened at now_ms. */
bool hu_voice_stream_tts_stale(int64_t now_ms, int64_t last_used_ms);

#endif /* HU_VOICE_STREAM_H */
