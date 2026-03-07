package ai.seaclaw.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.input.KeyboardType
import ai.seaclaw.app.GatewayManager

@Composable
fun SettingsScreen(gatewayManager: GatewayManager) {
    var url by remember { mutableStateOf(gatewayManager.gatewayUrl) }
    val isConnected by gatewayManager.isConnected.collectAsState()

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(SCTokens.spaceMd)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = SCTokens.spaceMd),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(SCTokens.spaceSm)
        ) {
            Box(
                modifier = Modifier
                    .size(SCTokens.spaceMd)
                    .clip(CircleShape)
                    .background(
                        color = if (isConnected) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.error
                    )
            )
            Text(
                text = if (isConnected) "Connected" else "Disconnected",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface
            )
        }
        OutlinedTextField(
            value = url,
            onValueChange = { url = it; gatewayManager.gatewayUrl = it },
            label = { Text("Gateway URL") },
            modifier = Modifier.fillMaxWidth(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
            singleLine = true
        )
        Text(
            text = "WebSocket URL, e.g. wss://10.0.2.2:3000/ws",
            modifier = Modifier.padding(top = SCTokens.spaceSm),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(
            modifier = Modifier.padding(top = SCTokens.spaceMd),
            horizontalArrangement = Arrangement.spacedBy(SCTokens.spaceSm)
        ) {
            Button(
                onClick = {
                    if (isConnected) gatewayManager.disconnect()
                    else gatewayManager.connect()
                }
            ) {
                Text(if (isConnected) "Disconnect" else "Connect")
            }
            if (isConnected) {
                Button(
                    onClick = { gatewayManager.reconnect() }
                ) {
                    Text("Reconnect")
                }
            }
        }
    }
}
