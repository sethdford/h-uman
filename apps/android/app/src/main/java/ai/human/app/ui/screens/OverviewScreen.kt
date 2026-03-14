package ai.human.app.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import ai.human.app.ConnectionState
import ai.human.app.GatewayClient
import ai.human.app.ui.HUTokens

private val overviewSpring = spring<IntOffset>(
    dampingRatio = 0.7f,
    stiffness = HUTokens.springStandardStiffness,
)

@Composable
fun OverviewScreen(
    gateway: GatewayClient = GatewayClient(),
    connectionState: ConnectionState = ConnectionState.DISCONNECTED,
) {
    val colorScheme = MaterialTheme.colorScheme
    var visible by remember { mutableStateOf(false) }
    val events by gateway.events.collectAsState()
    val recentActivity = remember { mutableStateListOf<String>() }

    LaunchedEffect(Unit) {
        visible = true
    }

    LaunchedEffect(events) {
        events?.let { event ->
            val summary = when (event.type) {
                "message" -> "Message: ${event.payload?.optString("text", "")?.take(60) ?: ""}"
                "channel_event" -> "Channel: ${event.payload?.optString("channel", "unknown")}"
                "tool_call" -> "Tool: ${event.payload?.optString("name", "unknown")}"
                "status" -> "Status: ${event.payload?.optString("status", "")}"
                else -> "${event.type}: ${event.payload?.optString("text", "")?.take(40) ?: "event"}"
            }
            if (summary.isNotBlank()) {
                recentActivity.add(0, summary)
                if (recentActivity.size > 20) recentActivity.removeRange(20, recentActivity.size)
            }
        }
    }

    val statusLabel = when (connectionState) {
        ConnectionState.CONNECTED -> "Connected"
        ConnectionState.CONNECTING -> "Connecting..."
        ConnectionState.DISCONNECTED -> "Disconnected"
    }
    val statusColor = when (connectionState) {
        ConnectionState.CONNECTED -> colorScheme.primary
        ConnectionState.CONNECTING -> colorScheme.tertiary
        ConnectionState.DISCONNECTED -> colorScheme.error
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = HUTokens.spaceMd)
            .padding(top = HUTokens.spaceMd, bottom = HUTokens.spaceLg),
        verticalArrangement = Arrangement.spacedBy(HUTokens.spaceMd),
        contentPadding = PaddingValues(bottom = HUTokens.space2xl),
    ) {
        item {
            AnimatedVisibility(
                visible = visible,
                enter = fadeIn(animationSpec = tween(HUTokens.durationNormal.toInt())) +
                    slideInVertically(
                        animationSpec = overviewSpring,
                        initialOffsetY = { it / 4 },
                    ),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = "Welcome back",
                        style = MaterialTheme.typography.headlineLarge,
                        color = colorScheme.onBackground,
                    )
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(HUTokens.spaceXs),
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.semantics {
                            contentDescription = "Gateway status: $statusLabel"
                        },
                    ) {
                        Box(
                            modifier = Modifier
                                .size(8.dp)
                                .clip(CircleShape)
                                .background(statusColor),
                        )
                        Text(
                            text = statusLabel,
                            style = MaterialTheme.typography.labelSmall,
                            color = colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }

        item {
            Spacer(modifier = Modifier.height(HUTokens.spaceSm))
        }

        item {
            AnimatedVisibility(
                visible = visible,
                enter = fadeIn(animationSpec = tween(HUTokens.durationNormal.toInt(), delayMillis = 50)) +
                    slideInVertically(
                        animationSpec = overviewSpring,
                        initialOffsetY = { it / 4 },
                    ),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(HUTokens.spaceMd),
                ) {
                    StatCard(
                        title = "Gateway",
                        value = statusLabel,
                        modifier = Modifier.weight(1f),
                    )
                    StatCard(
                        title = "Events",
                        value = "${recentActivity.size}",
                        modifier = Modifier.weight(1f),
                    )
                }
            }
        }

        item {
            AnimatedVisibility(
                visible = visible,
                enter = fadeIn(animationSpec = tween(HUTokens.durationNormal.toInt(), delayMillis = 100)) +
                    slideInVertically(
                        animationSpec = overviewSpring,
                        initialOffsetY = { it / 4 },
                    ),
            ) {
                StatCard(
                    title = "Connection",
                    value = if (connectionState == ConnectionState.CONNECTED) "Active" else "Offline",
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }

        item {
            Spacer(modifier = Modifier.height(HUTokens.spaceLg))
        }

        item {
            AnimatedVisibility(
                visible = visible,
                enter = fadeIn(animationSpec = tween(HUTokens.durationNormal.toInt(), delayMillis = 150)) +
                    slideInVertically(
                        animationSpec = overviewSpring,
                        initialOffsetY = { it / 4 },
                    ),
            ) {
                Text(
                    text = if (recentActivity.isEmpty()) "No activity yet" else "Live activity",
                    style = MaterialTheme.typography.titleMedium,
                    color = colorScheme.onBackground,
                )
            }
        }

        items(recentActivity) { activity ->
            AnimatedVisibility(
                visible = visible,
                enter = fadeIn(animationSpec = tween(HUTokens.durationNormal.toInt(), delayMillis = 200)) +
                    slideInVertically(
                        animationSpec = spring<IntOffset>(
                            dampingRatio = 0.7f,
                            stiffness = HUTokens.springStandardStiffness,
                        ),
                        initialOffsetY = { it / 4 },
                    ),
            ) {
                ActivityItem(text = activity)
            }
        }
    }
}

@Composable
private fun StatCard(
    title: String,
    value: String,
    modifier: Modifier = Modifier,
) {
    val colorScheme = MaterialTheme.colorScheme
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(HUTokens.radiusLg))
            .background(colorScheme.primaryContainer)
            .padding(HUTokens.spaceMd)
            .semantics { contentDescription = "$title: $value" },
    ) {
        Column {
            Text(
                text = title,
                style = MaterialTheme.typography.labelMedium,
                color = colorScheme.onSurfaceVariant,
            )
            Spacer(modifier = Modifier.height(HUTokens.spaceXs))
            Text(
                text = value,
                style = MaterialTheme.typography.titleLarge,
                color = colorScheme.onBackground,
            )
        }
    }
}

@Composable
private fun ActivityItem(text: String) {
    val colorScheme = MaterialTheme.colorScheme
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(HUTokens.radiusMd))
            .background(colorScheme.primaryContainer)
            .padding(HUTokens.spaceMd),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium,
            color = colorScheme.onBackground,
        )
    }
}
