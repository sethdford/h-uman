package ai.human.app

import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Psychology
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.text.style.TextAlign
import ai.human.app.ui.screens.ChatScreen
import ai.human.app.ui.screens.MemoryScreen
import ai.human.app.ui.screens.OverviewScreen
import ai.human.app.ui.screens.SessionsScreen
import ai.human.app.ui.screens.SettingsScreen
import ai.human.app.ui.screens.ToolsScreen
import ai.human.app.data.readGatewayUrl
import ai.human.app.data.saveGatewayUrl
import ai.human.app.ui.theme.HumanTheme
import ai.human.app.ui.HUTokens
import ai.human.app.ui.glassSurface
import ai.human.app.R
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            HumanTheme {
                HumanRoot(
                    intent = intent,
                    initialSkipOnboarding = intent.getBooleanExtra(EXTRA_SKIP_ONBOARDING_FOR_TEST, false),
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
    }
}

private fun isValidGatewayUrl(url: String): Boolean {
    val trimmed = url.trim()
    if (trimmed.isEmpty()) return false
    return trimmed.startsWith("ws://") || trimmed.startsWith("wss://") ||
        trimmed.startsWith("http://") || trimmed.startsWith("https://")
}

/** Instrumented tests pass this extra to reach main nav without onboarding. */
const val EXTRA_SKIP_ONBOARDING_FOR_TEST: String = "ai.human.SKIP_ONBOARDING_FOR_TEST"

private object MainRoutes {
    const val Overview = "overview"
    const val Chat = "chat"
    const val Memory = "memory"
    const val Sessions = "sessions"
    const val Tools = "tools"
    const val Settings = "settings"
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HumanRoot(intent: Intent?, initialSkipOnboarding: Boolean = false) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var prefsLoaded by remember { mutableStateOf(false) }
    var gatewayUrl by rememberSaveable { mutableStateOf("http://localhost:3000") }
    var hasOnboarded by rememberSaveable { mutableStateOf(initialSkipOnboarding) }

    LaunchedEffect(Unit) {
        gatewayUrl = ctx.readGatewayUrl()
        prefsLoaded = true
    }

    if (!prefsLoaded) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
        }
        return
    }

    if (hasOnboarded) {
        HumanApp(intent = intent, initialGatewayUrl = gatewayUrl)
    } else {
        OnboardingScreen(
            initialUrl = gatewayUrl,
            onComplete = { url ->
                gatewayUrl = url.trim()
                scope.launch {
                    ctx.saveGatewayUrl(gatewayUrl)
                }
                hasOnboarded = true
            },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HumanApp(intent: Intent?, initialGatewayUrl: String = "http://localhost:3000") {
    val vm: MainViewModel = viewModel()
    val gateway = vm.gateway
    val navController = rememberNavController()
    val navBackStackEntry by navController.currentBackStackEntryAsState()
    val currentRoute = navBackStackEntry?.destination?.route ?: MainRoutes.Overview
    val connectionState by gateway.state.collectAsState()

    fun navigateTo(route: String) {
        navController.navigate(route) {
            popUpTo(navController.graph.startDestinationId) {
                saveState = true
            }
            launchSingleTop = true
            restoreState = true
        }
    }

    DisposableEffect(currentRoute) {
        if (currentRoute in listOf(MainRoutes.Overview, MainRoutes.Chat, MainRoutes.Memory, MainRoutes.Sessions, MainRoutes.Tools)) {
            gateway.connectIfNeeded(initialGatewayUrl)
        }
        if (currentRoute == MainRoutes.Overview && connectionState == ConnectionState.CONNECTED) {
            gateway.fetchOverviewData()
        }
        onDispose { }
    }

    LaunchedEffect(intent?.data) {
        val uri = intent?.data ?: return@LaunchedEffect
        if (uri.scheme == "human") {
            when (uri.host) {
                "chat" -> navigateTo(MainRoutes.Chat)
                "memory" -> navigateTo(MainRoutes.Memory)
                "sessions" -> navigateTo(MainRoutes.Sessions)
                "tools" -> navigateTo(MainRoutes.Tools)
                "settings" -> navigateTo(MainRoutes.Settings)
                "overview" -> navigateTo(MainRoutes.Overview)
            }
        }
    }

    LaunchedEffect(currentRoute) {
        if (connectionState != ConnectionState.CONNECTED) return@LaunchedEffect
        when (currentRoute) {
            MainRoutes.Memory -> {
                gateway.prefetchMemory()
            }
            MainRoutes.Sessions -> {
                gateway.prefetchSessions()
                gateway.prefetchTools()
            }
            MainRoutes.Tools -> {
                gateway.prefetchSessions()
                gateway.prefetchTools()
            }
            MainRoutes.Overview -> {
                gateway.prefetchSessions()
                gateway.prefetchTools()
            }
            else -> { }
        }
    }

    BackHandler(enabled = currentRoute != MainRoutes.Overview) {
        navigateTo(MainRoutes.Overview)
    }

    Scaffold(
        bottomBar = {
            NavigationBar(
                containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
            ) {
                NavigationBarItem(
                    selected = currentRoute == MainRoutes.Overview,
                    onClick = { navigateTo(MainRoutes.Overview) },
                    icon = {
                        Icon(
                            painter = painterResource(R.drawable.ic_phosphor_house),
                            contentDescription = "Overview",
                            tint = if (currentRoute == MainRoutes.Overview) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    label = { Text("Overview") },
                )
                NavigationBarItem(
                    selected = currentRoute == MainRoutes.Chat,
                    onClick = { navigateTo(MainRoutes.Chat) },
                    icon = {
                        Icon(
                            painter = painterResource(R.drawable.ic_phosphor_chat_circle),
                            contentDescription = "Chat",
                            tint = if (currentRoute == MainRoutes.Chat) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    label = { Text("Chat") },
                )
                NavigationBarItem(
                    selected = currentRoute == MainRoutes.Memory,
                    onClick = { navigateTo(MainRoutes.Memory) },
                    icon = {
                        Icon(
                            imageVector = Icons.Outlined.Psychology,
                            contentDescription = "Memory",
                            tint = if (currentRoute == MainRoutes.Memory) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    label = { Text("Memory") },
                )
                NavigationBarItem(
                    selected = currentRoute == MainRoutes.Sessions,
                    onClick = { navigateTo(MainRoutes.Sessions) },
                    icon = {
                        Icon(
                            painter = painterResource(R.drawable.ic_phosphor_clock),
                            contentDescription = "Sessions",
                            tint = if (currentRoute == MainRoutes.Sessions) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    label = { Text("Sessions") },
                )
                NavigationBarItem(
                    selected = currentRoute == MainRoutes.Tools,
                    onClick = { navigateTo(MainRoutes.Tools) },
                    icon = {
                        Icon(
                            painter = painterResource(R.drawable.ic_phosphor_wrench),
                            contentDescription = "Tools",
                            tint = if (currentRoute == MainRoutes.Tools) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    label = { Text("Tools") },
                )
                NavigationBarItem(
                    selected = currentRoute == MainRoutes.Settings,
                    onClick = { navigateTo(MainRoutes.Settings) },
                    icon = {
                        Icon(
                            painter = painterResource(R.drawable.ic_phosphor_gear),
                            contentDescription = "Settings",
                            tint = if (currentRoute == MainRoutes.Settings) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    label = { Text("Settings") },
                )
            }
        },
    ) { padding ->
        Box(modifier = Modifier.padding(padding)) {
            NavHost(
                navController = navController,
                startDestination = MainRoutes.Overview,
                modifier = Modifier.fillMaxSize(),
            ) {
                composable(MainRoutes.Overview) {
                    OverviewScreen(gateway = gateway, connectionState = connectionState)
                }
                composable(MainRoutes.Chat) {
                    ChatScreen(gateway = gateway)
                }
                composable(MainRoutes.Memory) {
                    MemoryScreen(gateway = gateway, connectionState = connectionState)
                }
                composable(MainRoutes.Sessions) {
                    SessionsScreen(gateway = gateway, connectionState = connectionState)
                }
                composable(MainRoutes.Tools) {
                    ToolsScreen(gateway = gateway, connectionState = connectionState)
                }
                composable(MainRoutes.Settings) {
                    SettingsScreen(
                        gateway = gateway,
                        connectionState = connectionState,
                        initialGatewayUrl = initialGatewayUrl,
                    )
                }
            }
        }
    }
}

@Composable
private fun OnboardingScreen(
    initialUrl: String = "http://localhost:3000",
    onComplete: (String) -> Unit,
) {
    val colorScheme = MaterialTheme.colorScheme
    var gatewayUrl by remember { mutableStateOf(initialUrl) }
    val isValidUrl = isValidGatewayUrl(gatewayUrl)

    val pageIcons = listOf(
        R.drawable.ic_phosphor_house,
        R.drawable.ic_phosphor_lightning,
        R.drawable.ic_phosphor_chat_circle,
    )
    val pages = listOf(
        Triple("Welcome to h-uman", "not quite human.", "Minimal footprint, maximum capability."),
        // fp-template: Triple("Lightning Fast", "{{binary_mb}} binary, {{idle_rss_bound}} RAM.", "{{startup_bound}} startup. Zero dependencies."),
        Triple("Lightning Fast", "~3 MB binary, <9 MB RAM.", "<6 ms startup. Zero dependencies."),
        Triple("34 Channels", "Telegram, Discord, Slack, and more.", "Connect your preferred messaging platform."),
    )
    val pagerState = rememberPagerState(pageCount = { pages.size })

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { /* result not required for flow */ }

    LaunchedEffect(Unit) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            permissionLauncher.launch(android.Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(colorScheme.background)
            .padding(HUTokens.spaceLg),
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        HorizontalPager(
            state = pagerState,
            modifier = Modifier.weight(1f),
            userScrollEnabled = true,
        ) { page ->
            val pageOffset = (pagerState.currentPage - page) + pagerState.currentPageOffsetFraction
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .graphicsLayer {
                        alpha = 1f - kotlin.math.abs(pageOffset) * 0.4f
                        translationX = pageOffset * 80f
                    },
                verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Icon(
                    painter = painterResource(pageIcons[page]),
                    contentDescription = "Illustration",
                    modifier = Modifier.size(80.dp),
                    tint = colorScheme.primary,
                )
                Spacer(modifier = Modifier.height(HUTokens.spaceLg))
                Text(
                    text = pages[page].first,
                    style = MaterialTheme.typography.headlineLarge,
                    color = colorScheme.onBackground,
                    textAlign = TextAlign.Center,
                )
                Spacer(modifier = Modifier.height(HUTokens.spaceMd))
                Text(
                    text = pages[page].second,
                    style = MaterialTheme.typography.bodyLarge,
                    color = colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
                Text(
                    text = pages[page].third,
                    style = MaterialTheme.typography.bodyMedium,
                    color = colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
            }
        }

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .glassSurface()
                .padding(HUTokens.spaceMd),
            verticalArrangement = Arrangement.spacedBy(HUTokens.spaceMd),
        ) {
            BasicTextField(
                value = gatewayUrl,
                onValueChange = { gatewayUrl = it },
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(HUTokens.radiusMd))
                    .background(colorScheme.surfaceContainerHigh)
                    .border(
                        HUTokens.spaceXs / 4,
                        if (isValidUrl) colorScheme.outlineVariant else colorScheme.error,
                        RoundedCornerShape(HUTokens.radiusMd),
                    )
                    .padding(horizontal = HUTokens.spaceMd, vertical = HUTokens.spaceSm)
                    .semantics { contentDescription = "Gateway URL" },
                textStyle = TextStyle(
                    color = colorScheme.onSurface,
                    fontSize = HUTokens.textBase,
                ),
                singleLine = true,
                cursorBrush = SolidColor(colorScheme.primary),
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(HUTokens.radiusMd))
                    .background(
                        if (isValidUrl) colorScheme.primary
                        else colorScheme.surfaceContainerHigh,
                    )
                    .then(
                        if (isValidUrl) Modifier.clickable { onComplete(gatewayUrl) }
                        else Modifier,
                    )
                    .padding(HUTokens.spaceMd)
                    .semantics {
                        contentDescription = if (isValidUrl) "Get started" else "Get started (enter valid URL first)"
                    },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = "Get Started",
                    style = MaterialTheme.typography.labelLarge,
                    color = if (isValidUrl) colorScheme.onPrimary else colorScheme.onSurfaceVariant,
                )
            }

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onComplete("http://localhost:3000") }
                    .padding(HUTokens.spaceSm),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = "Skip for now",
                    style = MaterialTheme.typography.bodySmall,
                    color = colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
