package ai.human.app.ui

import android.os.Build
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.BlurEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.TileMode
import androidx.compose.ui.graphics.graphicsLayer

enum class GlassTier { Subtle, Standard, Prominent }

@Composable
fun SCGlassSurface(
    modifier: Modifier = Modifier,
    tier: GlassTier = GlassTier.Standard,
    content: @Composable BoxScope.() -> Unit
) {
    val radius = when (tier) {
        GlassTier.Subtle -> HUTokens.radiusMd
        GlassTier.Standard -> HUTokens.radiusLg
        GlassTier.Prominent -> HUTokens.radiusXl
    }
    val bgAlpha = when (tier) {
        GlassTier.Subtle -> 0.04f
        GlassTier.Standard -> 0.06f
        GlassTier.Prominent -> 0.08f
    }
    val blurRadius = when (tier) {
        GlassTier.Subtle -> 12f
        GlassTier.Standard -> 24f
        GlassTier.Prominent -> 32f
    }
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(radius))
            .then(
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    Modifier.graphicsLayer {
                        renderEffect = BlurEffect(blurRadius, blurRadius, TileMode.Clamp)
                    }
                } else Modifier
            )
            .background(Color.White.copy(alpha = bgAlpha)),
        content = content
    )
}
