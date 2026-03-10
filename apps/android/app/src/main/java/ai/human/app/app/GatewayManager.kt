package ai.human.app

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

private const val PREFS_NAME = "human_gateway"
private const val KEY_GATEWAY_URL = "hu_gateway_url"
private const val DEFAULT_GATEWAY_URL = "wss://10.0.2.2:3000/ws"

/**
 * Wraps GatewayClient and manages connection state + gateway URL.
 */
class GatewayManager(context: Context) {

    private val prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var client: GatewayClient? = null
    private val _events = MutableSharedFlow<GatewayClient.Event>()
    private val _isConnected = MutableStateFlow(false)

    var gatewayUrl: String
        get() = prefs.getString(KEY_GATEWAY_URL, DEFAULT_GATEWAY_URL) ?: DEFAULT_GATEWAY_URL
        set(value) {
            prefs.edit().putString(KEY_GATEWAY_URL, value).apply()
        }

    val isConnected: StateFlow<Boolean>
        get() = _isConnected.asStateFlow()

    val events: Flow<GatewayClient.Event>
        get() = _events.asSharedFlow()

    fun connect() {
        client?.disconnect()
        _isConnected.value = false
        client = GatewayClient(gatewayUrl).also { c ->
            scope.launch {
                c.events.collect { event ->
                    when (event) {
                        is GatewayClient.Event.Connected -> _isConnected.value = true
                        is GatewayClient.Event.Disconnected -> _isConnected.value = false
                        else -> {}
                    }
                    _events.emit(event)
                }
            }
            c.connect()
        }
    }

    fun disconnect() {
        client?.disconnect()
        client = null
        _isConnected.value = false
    }

    fun reconnect() {
        disconnect()
        connect()
    }

    suspend fun request(method: String, params: Map<String, Any?> = emptyMap()): Result<org.json.JSONObject> {
        val c = client ?: return Result.failure(IllegalStateException("Not connected"))
        return c.request(method, params)
    }

    fun registerPushToken(token: String) {
        client?.registerPushToken(token)
    }

    fun unregisterPushToken(token: String) {
        client?.unregisterPushToken(token)
    }
}
