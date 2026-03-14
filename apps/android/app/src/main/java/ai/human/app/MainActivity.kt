package ai.human.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Chat
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import ai.human.app.ui.screens.ChatScreen
import ai.human.app.ui.screens.OverviewScreen
import ai.human.app.ui.screens.SessionsScreen
import ai.human.app.ui.screens.SettingsScreen
import ai.human.app.ui.screens.ToolsScreen
import ai.human.app.ui.theme.HumanTheme

/**
 * Main activity with edge-to-edge display.
 * Predictive back gesture is supported via android:enableOnBackInvokedCallback="true"
 * in AndroidManifest.xml — the default back-to-home animation is used without
 * intercepting at the root activity.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            HumanTheme {
                HumanApp()
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HumanApp() {
    var selectedTab by remember { mutableIntStateOf(0) }
    val gateway = remember { GatewayClient() }
    val connectionState by gateway.state.collectAsState()

    DisposableEffect(Unit) {
        gateway.connect("http://localhost:3000")
        onDispose { gateway.disconnect() }
    }

    Scaffold(
        bottomBar = {
            NavigationBar(
                containerColor = MaterialTheme.colorScheme.surface,
            ) {
                NavigationBarItem(
                    selected = selectedTab == 0,
                    onClick = { selectedTab = 0 },
                    icon = { Icon(Icons.Filled.Home, contentDescription = null) },
                    label = { Text("Overview") },
                )
                NavigationBarItem(
                    selected = selectedTab == 1,
                    onClick = { selectedTab = 1 },
                    icon = { Icon(Icons.Filled.Chat, contentDescription = null) },
                    label = { Text("Chat") },
                )
                NavigationBarItem(
                    selected = selectedTab == 2,
                    onClick = { selectedTab = 2 },
                    icon = { Icon(Icons.Filled.History, contentDescription = null) },
                    label = { Text("Sessions") },
                )
                NavigationBarItem(
                    selected = selectedTab == 3,
                    onClick = { selectedTab = 3 },
                    icon = { Icon(Icons.Filled.Build, contentDescription = null) },
                    label = { Text("Tools") },
                )
                NavigationBarItem(
                    selected = selectedTab == 4,
                    onClick = { selectedTab = 4 },
                    icon = { Icon(Icons.Filled.Settings, contentDescription = null) },
                    label = { Text("Settings") },
                )
            }
        },
    ) { padding ->
        Box(modifier = Modifier.padding(padding)) {
            AnimatedContent(
                targetState = selectedTab,
                transitionSpec = {
                    fadeIn() togetherWith fadeOut()
                },
                label = "screen_transition",
            ) { tab ->
                when (tab) {
                    0 -> OverviewScreen(gateway = gateway, connectionState = connectionState)
                    1 -> ChatScreen(gateway = gateway)
                    2 -> SessionsScreen()
                    3 -> ToolsScreen()
                    4 -> SettingsScreen()
                    else -> OverviewScreen(gateway = gateway, connectionState = connectionState)
                }
            }
        }
    }
}
