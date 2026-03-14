package ai.human.app.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.spring
import androidx.compose.animation.fadeIn
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.IntOffset
import ai.human.app.ui.HUTokens

private data class ToolItem(
    val id: String,
    val name: String,
    val description: String,
    val icon: androidx.compose.ui.graphics.vector.ImageVector,
)

private data class ToolCategory(
    val title: String,
    val tools: List<ToolItem>,
)

private val sessionSpring = spring<IntOffset>(
    dampingRatio = 0.7f,
    stiffness = HUTokens.springStandardStiffness,
)

@Composable
fun ToolsScreen() {
    val colorScheme = MaterialTheme.colorScheme
    val categories = remember {
        listOf(
            ToolCategory(
                title = "Communication",
                tools = listOf(
                    ToolItem("1", "Slack", "Send and receive Slack messages", Icons.Filled.Build),
                    ToolItem("2", "Email", "Read and send emails", Icons.Filled.Build),
                ),
            ),
            ToolCategory(
                title = "Development",
                tools = listOf(
                    ToolItem("3", "Shell", "Execute shell commands", Icons.Filled.Terminal),
                    ToolItem("4", "Code Search", "Search across codebases", Icons.Filled.Search),
                    ToolItem("5", "Git", "Run git operations", Icons.Filled.Code),
                ),
            ),
            ToolCategory(
                title = "Utilities",
                tools = listOf(
                    ToolItem("6", "Web Search", "Search the web", Icons.Filled.Search),
                    ToolItem("7", "Calculator", "Perform calculations", Icons.Filled.Build),
                ),
            ),
        )
    }
    var visible by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        visible = true
    }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = HUTokens.spaceMd)
            .padding(top = HUTokens.spaceMd, bottom = HUTokens.spaceLg),
        verticalArrangement = Arrangement.spacedBy(HUTokens.spaceLg),
        contentPadding = PaddingValues(bottom = HUTokens.space2xl),
    ) {
        item {
            AnimatedVisibility(
                visible = visible,
                enter = fadeIn(animationSpec = spring(dampingRatio = 0.7f)) +
                    slideInVertically(
                        animationSpec = sessionSpring,
                        initialOffsetY = { it / 4 },
                    ),
            ) {
                Text(
                    text = "Tools",
                    style = MaterialTheme.typography.headlineLarge,
                    color = colorScheme.onBackground,
                )
            }
        }

        categories.forEachIndexed { index, category ->
            item(key = "header_${category.title}") {
                AnimatedVisibility(
                    visible = visible,
                    enter = fadeIn(animationSpec = spring(dampingRatio = 0.7f)) +
                        slideInVertically(
                            animationSpec = sessionSpring,
                            initialOffsetY = { it / 4 },
                        ),
                ) {
                    Text(
                        text = category.title,
                        style = MaterialTheme.typography.titleMedium,
                        color = colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = HUTokens.spaceMd),
                    )
                }
            }

            item(key = "grid_${category.title}") {
                AnimatedVisibility(
                    visible = visible,
                    enter = fadeIn(animationSpec = spring(dampingRatio = 0.7f)) +
                        slideInVertically(
                            animationSpec = sessionSpring,
                            initialOffsetY = { it / 4 },
                        ),
                ) {
                    Column(
                        verticalArrangement = Arrangement.spacedBy(HUTokens.spaceMd),
                    ) {
                        category.tools.chunked(2).forEach { rowTools ->
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(HUTokens.spaceMd),
                            ) {
                                rowTools.forEach { tool ->
                                    Box(modifier = Modifier.weight(1f)) {
                                        ToolCard(
                                            name = tool.name,
                                            description = tool.description,
                                            icon = tool.icon,
                                        )
                                    }
                                }
                                if (rowTools.size == 1) {
                                    Spacer(modifier = Modifier.weight(1f))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ToolCard(
    name: String,
    description: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
) {
    val colorScheme = MaterialTheme.colorScheme

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(HUTokens.radiusLg))
            .background(colorScheme.primaryContainer)
            .padding(HUTokens.spaceMd),
    ) {
        Column {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = colorScheme.primary,
                modifier = Modifier.size(HUTokens.spaceLg),
            )
            Text(
                text = name,
                style = MaterialTheme.typography.titleSmall,
                color = colorScheme.onSurface,
                modifier = Modifier.padding(top = HUTokens.spaceSm),
            )
            Text(
                text = description,
                style = MaterialTheme.typography.bodySmall,
                color = colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = HUTokens.spaceXs),
            )
        }
    }
}
