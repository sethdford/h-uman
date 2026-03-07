package ai.seaclaw.app.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Send
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import ai.seaclaw.app.ui.SCTokens
import ai.seaclaw.app.GatewayClient
import ai.seaclaw.app.GatewayManager
import kotlinx.coroutines.launch

@Composable
fun ChatScreen(gatewayManager: GatewayManager) {
    val messages = remember { mutableStateListOf<ChatMessage>() }
    val toolCalls = remember { mutableStateListOf<ToolCallItem>() }
    var inputText by remember { mutableStateOf("") }
    var errorBanner by remember { mutableStateOf<String?>(null) }
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()

    LaunchedEffect(Unit) {
        gatewayManager.connect()
    }

    DisposableEffect(Unit) {
        onDispose { gatewayManager.disconnect() }
    }

    LaunchedEffect(messages.size, toolCalls.size) {
        if (messages.isNotEmpty() || toolCalls.isNotEmpty()) {
            listState.animateScrollToItem((messages.size + toolCalls.size - 1).coerceAtLeast(0))
        }
    }

    LaunchedEffect(gatewayManager) {
        gatewayManager.events.collect { e ->
            when (e) {
            is GatewayClient.Event.ServerEvent -> {
                when (e.event) {
                    "error" -> {
                        val msg = e.payload?.optString("message", "") ?: e.payload?.optString("error", "") ?: "Unknown error"
                        if (msg.isNotEmpty()) errorBanner = msg
                    }
                    "health" -> {
                        // Connection status updated via health event
                    }
                    "chat" -> {
                        val state = e.payload?.optString("state", "") ?: ""
                        val content = e.payload?.optString("message", "") ?: ""
                        if (content.isNotEmpty()) {
                            when (state) {
                                "received" -> messages.add(ChatMessage(text = content, role = Role.USER))
                                "sent", "chunk" -> {
                                    val last = messages.lastOrNull()
                                    if (last != null && last.role == Role.ASSISTANT && state == "chunk") {
                                        messages[messages.lastIndex] = last.copy(text = last.text + content)
                                    } else {
                                        messages.add(ChatMessage(text = content, role = Role.ASSISTANT))
                                    }
                                }
                                else -> {}
                            }
                        }
                    }
                    "agent.tool" -> {
                        val name = e.payload?.optString("message", "") ?: e.payload?.optString("tool", "") ?: e.payload?.optString("name", "") ?: "tool"
                        if (name.isNotEmpty()) {
                            val args = e.payload?.optJSONObject("arguments")?.toString()
                            val hasSuccess = e.payload?.has("success") == true
                            if (hasSuccess && toolCalls.isNotEmpty()) {
                                val idx = toolCalls.lastIndex
                                val last = toolCalls[idx]
                                val ok = e.payload?.optBoolean("success", false)
                                val result = e.payload?.opt("detail")?.toString() ?: e.payload?.opt("message")?.toString()
                                toolCalls[idx] = last.copy(
                                    status = if (ok) ToolStatus.COMPLETED else ToolStatus.FAILED,
                                    result = result
                                )
                            } else {
                                toolCalls.add(ToolCallItem(name = name, arguments = args, status = ToolStatus.RUNNING))
                            }
                        }
                    }
                    else -> {}
                }
            }
            else -> {}
        }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(SCTokens.spaceMd)
    ) {
        errorBanner?.let { msg ->
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                shape = RoundedCornerShape(SCTokens.radiusMd),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = SCTokens.spaceSm)
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(SCTokens.spaceMd),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(msg, color = MaterialTheme.colorScheme.onErrorContainer)
                    TextButton(onClick = { errorBanner = null }) {
                        Text("Dismiss")
                    }
                }
            }
        }
        LazyColumn(
            state = listState,
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(SCTokens.spaceSm)
        ) {
            itemsIndexed(messages, key = { index, _ -> "msg_$index" }) { _, msg ->
                AnimatedVisibility(
                    visible = true,
                    enter = fadeIn(animationSpec = tween(SCTokens.durationNormal.toInt())) +
                        slideInVertically(animationSpec = tween(SCTokens.durationNormal.toInt()), initialOffsetY = { it }),
                    exit = fadeOut(animationSpec = tween(SCTokens.durationFast.toInt()))
                ) {
                    ChatBubble(
                        text = msg.text,
                        role = msg.role
                    )
                }
            }
            itemsIndexed(toolCalls, key = { index, _ -> "tc_$index" }) { _, tc ->
                ToolCallCard(
                    name = tc.name,
                    arguments = tc.arguments,
                    status = tc.status,
                    result = tc.result
                )
            }
        }

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = SCTokens.spaceSm),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedTextField(
                value = inputText,
                onValueChange = { inputText = it },
                modifier = Modifier
                    .weight(1f)
                    .padding(end = SCTokens.spaceSm),
                placeholder = { Text("Message") },
                textStyle = MaterialTheme.typography.bodyLarge,
                singleLine = true,
                shape = RoundedCornerShape(SCTokens.radiusLg)
            )
            IconButton(
                onClick = {
                    val trimmed = inputText.trim()
                    if (trimmed.isNotEmpty()) {
                        messages.add(ChatMessage(text = trimmed, role = Role.USER))
                        inputText = ""
                        scope.launch {
                            gatewayManager.request("chat.send", mapOf("message" to trimmed))
                        }
                    }
                }
            ) {
                Icon(Icons.Default.Send, contentDescription = "Send", tint = MaterialTheme.colorScheme.primary)
            }
        }
    }
}

@Composable
private fun ChatBubble(
    text: String,
    role: Role,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = if (role == Role.USER) Arrangement.End else Arrangement.Start
    ) {
        Card(
            colors = CardDefaults.cardColors(
                containerColor = if (role == Role.USER) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant
            ),
            shape = RoundedCornerShape(SCTokens.radiusXl)
        ) {
            Text(
                text = text,
                modifier = Modifier.padding(SCTokens.spaceMd),
                style = MaterialTheme.typography.bodyLarge,
                color = if (role == Role.USER) Color.White else MaterialTheme.colorScheme.onSurface
            )
        }
    }
}

@Composable
private fun ToolCallCard(
    name: String,
    arguments: String?,
    status: ToolStatus,
    result: String?,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(SCTokens.radiusLg)
    ) {
            Column(modifier = Modifier.padding(SCTokens.spaceMd)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(name, style = MaterialTheme.typography.titleSmall)
                Spacer(modifier = Modifier.weight(1f))
                if (status == ToolStatus.RUNNING) {
                    CircularProgressIndicator(
                        modifier = Modifier.height(SCTokens.spaceMd),
                        strokeWidth = SCTokens.radiusSm
                    )
                }
            }
            if (!arguments.isNullOrEmpty()) {
                Spacer(modifier = Modifier.height(SCTokens.spaceXs))
                Text(arguments, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (!result.isNullOrEmpty()) {
                Spacer(modifier = Modifier.height(SCTokens.spaceXs))
                Text(result, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

private data class ChatMessage(val text: String, val role: Role)
private enum class Role { USER, ASSISTANT }
private data class ToolCallItem(
    val name: String,
    val arguments: String?,
    val status: ToolStatus,
    val result: String? = null
)
private enum class ToolStatus { RUNNING, COMPLETED, FAILED }
