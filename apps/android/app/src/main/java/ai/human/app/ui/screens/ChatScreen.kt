package ai.human.app.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.spring
import androidx.compose.animation.fadeIn
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.IntOffset
import ai.human.app.GatewayClient
import ai.human.app.ui.HUTokens

private data class ChatMessage(
    val id: Int,
    val text: String,
    val isUser: Boolean,
)

private val chatSpring = spring<IntOffset>(
    dampingRatio = 0.7f,
    stiffness = HUTokens.springStandardStiffness,
)

@Composable
fun ChatScreen(gateway: GatewayClient = GatewayClient()) {
    val colorScheme = MaterialTheme.colorScheme
    var nextId = remember { 1 }
    val messages = remember { mutableStateListOf<ChatMessage>() }
    var inputText by remember { mutableStateOf("") }
    val listState = rememberLazyListState()
    val events by gateway.events.collectAsState()

    LaunchedEffect(events) {
        events?.let { event ->
            if (event.type == "message" || event.type == "response") {
                val text = event.payload?.optString("text", "")
                    ?: event.payload?.optString("content", "")
                    ?: ""
                if (text.isNotBlank()) {
                    messages.add(ChatMessage(nextId++, text, false))
                }
            }
        }
    }

    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) {
            listState.animateScrollToItem(messages.size - 1)
        }
    }

    Box(modifier = Modifier.fillMaxSize()) {
        Column(modifier = Modifier.fillMaxSize()) {
            LazyColumn(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(horizontal = HUTokens.spaceMd)
                    .padding(top = HUTokens.spaceMd),
                state = listState,
                verticalArrangement = Arrangement.spacedBy(HUTokens.spaceMd),
                contentPadding = PaddingValues(bottom = HUTokens.spaceMd),
            ) {
                items(messages) { message ->
                    AnimatedVisibility(
                        visible = true,
                        enter = fadeIn() + slideInVertically(
                            animationSpec = chatSpring,
                            initialOffsetY = { it / 4 },
                        ),
                    ) {
                        ChatBubble(
                            text = message.text,
                            isUser = message.isUser,
                        )
                    }
                }
            }

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(HUTokens.spaceMd)
                    .clip(RoundedCornerShape(HUTokens.radiusLg))
                    .background(colorScheme.surfaceVariant)
                    .padding(HUTokens.spaceSm),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(HUTokens.spaceSm),
            ) {
                BasicTextField(
                    value = inputText,
                    onValueChange = { inputText = it },
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = HUTokens.spaceMd, vertical = HUTokens.spaceSm),
                    textStyle = TextStyle(
                        color = colorScheme.onSurface,
                        fontSize = HUTokens.textBase,
                        fontFamily = MaterialTheme.typography.bodyMedium.fontFamily,
                    ),
                    singleLine = false,
                    maxLines = 4,
                    cursorBrush = SolidColor(colorScheme.primary),
                    decorationBox = { innerTextField ->
                        Box {
                            if (inputText.isEmpty()) {
                                Text(
                                    text = "Type a message...",
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = colorScheme.onSurfaceVariant,
                                )
                            }
                            innerTextField()
                        }
                    },
                    onTextLayout = {},
                    keyboardActions = androidx.compose.foundation.text.KeyboardActions(
                        onSend = {
                            if (inputText.isNotBlank()) {
                                messages.add(ChatMessage(nextId++, inputText, true))
                                gateway.send("chat", mapOf("text" to inputText))
                                inputText = ""
                            }
                        },
                    ),
                    keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(
                        imeAction = androidx.compose.ui.text.input.ImeAction.Send,
                    ),
                )
            }
        }
    }
}

@Composable
private fun ChatBubble(
    text: String,
    isUser: Boolean,
) {
    val colorScheme = MaterialTheme.colorScheme
    val (backgroundColor, textColor) = if (isUser) {
        colorScheme.primary to colorScheme.onPrimary
    } else {
        colorScheme.primaryContainer to colorScheme.onSurface
    }

    val role = if (isUser) "You" else "Assistant"
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .semantics { contentDescription = "$role said: $text" },
        horizontalArrangement = if (isUser) Arrangement.End else Arrangement.Start,
    ) {
        Box(
            modifier = Modifier
                .clip(
                    RoundedCornerShape(
                        topStart = HUTokens.radiusLg,
                        topEnd = HUTokens.radiusLg,
                        bottomStart = if (isUser) HUTokens.radiusLg else HUTokens.radiusSm,
                        bottomEnd = if (isUser) HUTokens.radiusSm else HUTokens.radiusLg,
                    ),
                )
                .background(backgroundColor)
                .padding(horizontal = HUTokens.spaceMd, vertical = HUTokens.spaceSm),
        ) {
            Text(
                text = text,
                style = MaterialTheme.typography.bodyMedium,
                color = textColor,
            )
        }
    }
}
