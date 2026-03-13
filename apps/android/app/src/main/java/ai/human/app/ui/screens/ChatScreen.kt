package ai.human.app.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Spring
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import ai.human.app.ui.HUTokens

private data class ChatMessage(
    val id: Int,
    val text: String,
    val isUser: Boolean,
)

private val chatSpring = spring<Float>(
    dampingRatio = Spring.DampingRatioMediumBouncy,
    stiffness = HUTokens.springStandardStiffness,
)

@Composable
fun ChatScreen() {
    val colorScheme = MaterialTheme.colorScheme
    val messages = remember {
        mutableStateListOf(
            ChatMessage(1, "Hello! How can I help you today?", false),
            ChatMessage(2, "What's the weather like?", true),
            ChatMessage(3, "I don't have access to real-time weather data, but I can help you find weather information or suggest checking a weather app.", false),
        )
    }
    var inputText by remember { mutableStateOf("") }
    val listState = rememberLazyListState()

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

    Row(
        modifier = Modifier.fillMaxWidth(),
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
