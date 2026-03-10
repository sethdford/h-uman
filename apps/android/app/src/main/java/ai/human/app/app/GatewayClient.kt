package ai.human.app

import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * OkHttp WebSocket client implementing the Human gateway control protocol.
 */
class GatewayClient(private val url: String) {

    sealed class Event {
        data class Connected(val helloOk: JSONObject) : Event()
        data class Response(val id: String, val ok: Boolean, val payload: JSONObject?) : Event()
        data class ServerEvent(val event: String, val payload: JSONObject?) : Event()
        object Disconnected : Event()
    }

    private val client = OkHttpClient.Builder().build()
    private var webSocket: WebSocket? = null
    private val pendingRequests = ConcurrentHashMap<String, CompletableRequest>()
    private val eventChannel = Channel<Event>(Channel.UNLIMITED)
    private val mutex = Mutex()
    private val reconnectScheduler: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor()
    private var reconnectScheduled = false
    private var reconnectAllowed = true

    val events: Flow<Event> = eventChannel.receiveAsFlow()

    private data class CompletableRequest(
        val continuation: (Result<JSONObject>) -> Unit
    )

    fun connect() {
        synchronized(this) {
            reconnectAllowed = true
        }
        val request = Request.Builder()
            .url(url)
            .build()
        webSocket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                val connectReq = JSONObject().apply {
                    put("type", "req")
                    put("id", "connect-${UUID.randomUUID()}")
                    put("method", "connect")
                    put("params", JSONObject())
                }
                webSocket.send(connectReq.toString())
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                try {
                    val json = JSONObject(text)
                    val type = json.optString("type", "")
                    when (type) {
                        "res" -> {
                            val payload = json.optJSONObject("payload")
                            val payloadType = payload?.optString("type", "") ?: ""
                            if (payloadType == "hello-ok") {
                                eventChannel.trySend(Event.Connected(payload ?: JSONObject()))
                            }
                            val id = json.optString("id", "")
                            if (id.isNotEmpty()) {
                                val ok = json.optBoolean("ok", false)
                                pendingRequests.remove(id)?.continuation?.invoke(Result.success(JSONObject().apply {
                                    put("ok", ok)
                                    put("payload", payload ?: JSONObject())
                                }))
                                eventChannel.trySend(Event.Response(id, ok, payload))
                            }
                        }
                        "event" -> {
                            val event = json.optString("event", "")
                            val payload = json.optJSONObject("payload")
                            eventChannel.trySend(Event.ServerEvent(event, payload))
                        }
                    }
                } catch (_: Exception) {}
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                this@GatewayClient.webSocket = null
                eventChannel.trySend(Event.Disconnected)
                pendingRequests.values.forEach { it.continuation.invoke(Result.failure(t)) }
                pendingRequests.clear()
                scheduleReconnect()
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {}
            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                this@GatewayClient.webSocket = null
                eventChannel.trySend(Event.Disconnected)
                scheduleReconnect()
            }
        })
    }

    private fun scheduleReconnect() {
        synchronized(this) {
            if (reconnectScheduled) return
            if (!reconnectAllowed) return
            reconnectScheduled = true
        }
        reconnectScheduler.schedule({
            synchronized(this@GatewayClient) {
                reconnectScheduled = false
            }
            if (reconnectAllowed) {
                connect()
            }
        }, 3, TimeUnit.SECONDS)
    }

    fun disconnect() {
        synchronized(this) {
            reconnectAllowed = false
        }
        webSocket?.close(1000, "client disconnect")
        webSocket = null
        pendingRequests.values.forEach { it.continuation.invoke(Result.failure(Throwable("Disconnected"))) }
        pendingRequests.clear()
    }

    suspend fun request(method: String, params: Map<String, Any?> = emptyMap()): Result<JSONObject> =
        mutex.withLock {
            val ws = webSocket ?: return Result.failure(IllegalStateException("Not connected"))
            val id = "req-${UUID.randomUUID()}"
            val req = JSONObject().apply {
                put("type", "req")
                put("id", id)
                put("method", method)
                put("params", JSONObject(params))
            }
            return suspendCoroutine { cont ->
                pendingRequests[id] = CompletableRequest { result ->
                    cont.resume(result)
                }
                ws.send(req.toString())
            }
        }

    fun sendMessage(json: String) {
        webSocket?.send(json)
    }

    fun registerPushToken(token: String) {
        val json = JSONObject().apply {
            put("action", "push.register")
            put("token", token)
            put("provider", "fcm")
        }
        sendMessage(json.toString())
    }

    fun unregisterPushToken(token: String) {
        val json = JSONObject().apply {
            put("action", "push.unregister")
            put("token", token)
        }
        sendMessage(json.toString())
    }
}
