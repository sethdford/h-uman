// Auto-generated from design-tokens/ — do not edit manually
package ai.seaclaw.app.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

object SCTokens {
    // Colors (Dark)
    object Dark {
        val accent = Color(0xFF14B8A6)
        val accentHover = Color(0xFF2DD4BF)
        val accentStrong = Color(0xFF5EEAD4)
        val accentSubtle = Color(0x2414B8A6)
        val backdropOverlay = Color(0x-67000000)
        val bg = Color(0xFF071018)
        val bgElevated = Color(0xFF152D45)
        val bgInset = Color(0xFF0B1A2A)
        val bgOverlay = Color(0xFF1B3754)
        val bgSurface = Color(0xFF0F2236)
        val border = Color(0xFF1E3A5C)
        val borderSubtle = Color(0xFF152D45)
        val disabledOverlay = Color(0x0AFFFFFF)
        val error = Color(0xFFF97066)
        val errorDim = Color(0x1FF97066)
        val focusRing = Color(0xFF14B8A6)
        val hoverOverlay = Color(0x0FFFFFFF)
        val info = Color(0xFF3B82F6)
        val infoDim = Color(0x263B82F6)
        val link = Color(0xFF2DD4BF)
        val linkActive = Color(0xFF14B8A6)
        val linkHover = Color(0xFF5EEAD4)
        val linkVisited = Color(0xFF0D9488)
        val onAccent = Color(0xFF071018)
        val pressedOverlay = Color(0x1AFFFFFF)
        val success = Color(0xFF10B981)
        val successDim = Color(0x2610B981)
        val text = Color(0xFFE4ECF4)
        val textFaint = Color(0xFF4A6A8A)
        val textMuted = Color(0xFF7A9AB8)
        val warning = Color(0xFFEAB308)
        val warningDim = Color(0x26EAB308)
    }

    // Colors (Light)
    object Light {
        val accent = Color(0xFF14B8A6)
        val accentHover = Color(0xFF0F766E)
        val accentLight = Color(0xFF99F6E4)
        val accentStrong = Color(0xFF14B8A6)
        val accentSubtle = Color(0x1A0D9488)
        val backdropOverlay = Color(0x4D000000)
        val bg = Color(0xFFF6F9FC)
        val bgElevated = Color(0xFFF0F4F8)
        val bgInset = Color(0xFFEAF0F6)
        val bgOverlay = Color(0xFFFFFFFF)
        val bgSurface = Color(0xFFFFFFFF)
        val bgWarm = Color(0xFFFAFCFE)
        val border = Color(0xFFD4DFE9)
        val borderSubtle = Color(0xFFF0F4F8)
        val disabledOverlay = Color(0x0A000000)
        val error = Color(0xFFE11D48)
        val errorDim = Color(0x14E11D48)
        val focusRing = Color(0xFF0D9488)
        val hoverOverlay = Color(0x0A000000)
        val info = Color(0xFF2563EB)
        val infoDim = Color(0x1A2563EB)
        val link = Color(0xFF0D9488)
        val linkActive = Color(0xFF14B8A6)
        val linkHover = Color(0xFF0F766E)
        val linkVisited = Color(0xFF0F766E)
        val onAccent = Color(0xFFFFFFFF)
        val pressedOverlay = Color(0x14000000)
        val success = Color(0xFF059669)
        val successDim = Color(0x1A059669)
        val text = Color(0xFF0B1A2A)
        val textFaint = Color(0xFF94ADC4)
        val textMuted = Color(0xFF4A6A8A)
        val warning = Color(0xFFCA8A04)
        val warningDim = Color(0x1ACA8A04)
    }

    // Spacing
    val spaceXs = 4.dp
    val spaceSm = 8.dp
    val spaceMd = 16.dp
    val spaceLg = 24.dp
    val spaceXl = 32.dp
    val space2xl = 48.dp

    // Radius
    val radiusSm = 4.dp
    val radiusMd = 8.dp
    val radiusLg = 12.dp
    val radiusXl = 16.dp

    // Font sizes
    val textXs = 11.sp
    val textSm = 13.sp
    val textBase = 14.sp
    val textLg = 16.sp
    val textXl = 20.sp

    // Font weights
    val weightBlack = 900
    val weightBold = 700
    val weightLight = 300
    val weightMedium = 500
    val weightNormal = 400
    val weightSemibold = 600

    // Line heights
    val leadingNone = 1f
    val leadingNormal = 1.5f
    val leadingRelaxed = 1.75f
    val leadingSnug = 1.375f
    val leadingTight = 1.25f

    // Letter spacing (em multiplier for LetterSpacing.Em())
    val tracking2xl = -0.02f
    val trackingBase = 0f
    val trackingHero = -0.04f
    val trackingLg = 0f
    val trackingSm = 0.01f
    val trackingXl = -0.01f
    val trackingXs = 0.02f

    // Duration (milliseconds)
    val durationFast = 100L
    val durationInstant = 50L
    val durationModerate = 300L
    val durationNormal = 200L
    val durationSlow = 350L
    val durationSlower = 500L
    val durationSlowest = 700L

    // Easing curves (CSS values for documentation)
    val easingEaseIn = "cubic-bezier(0.55, 0, 1, 0.45)"
    val easingEaseInOut = "cubic-bezier(0.65, 0, 0.35, 1)"
    val easingEaseOut = "cubic-bezier(0.16, 1, 0.3, 1)"
    val easingEmphasize = "cubic-bezier(0.2, 0, 0, 1)"
    val easingLinear = "linear"
    val easingSpringBounce = "linear(0, 0.004, 0.016, 0.035 4%, 0.147 8%, 0.51 17.2%, 0.726 23.1%, 0.884 28.9%, 0.976 34%, 1.019 38.3%, 1.045 42.5%, 1.054 47.7%, 1.04 56.7%, 1.017 64.3%, 1.003 76%, 1)"
    val easingSpringOut = "linear(0, 0.006, 0.025 2.8%, 0.101 6.1%, 0.539 18.9%, 0.721 25.3%, 0.849 31.5%, 0.937 38.1%, 0.968 41.8%, 0.991 45.7%, 1.006 50.1%, 1.015 55%, 1.017 63.9%, 1.001 85.9%, 1)"

    // Spring (stiffness, damping, mass)
    val springMicroStiffness = 400f
    val springMicroDamping = 30f
    val springMicroMass = 1f
    val springStandardStiffness = 200f
    val springStandardDamping = 20f
    val springStandardMass = 1f
    val springExpressiveStiffness = 120f
    val springExpressiveDamping = 14f
    val springExpressiveMass = 1f
    val springDramaticStiffness = 80f
    val springDramaticDamping = 10f
    val springDramaticMass = 1f

    // Opacity
    val opacityDisabled = 0.38f
    val opacityDragged = 0.16f
    val opacityFocus = 0.12f
    val opacityHover = 0.08f
    val opacityOverlayHeavy = 0.64f
    val opacityOverlayLight = 0.08f
    val opacityOverlayMedium = 0.32f
    val opacityPressed = 0.12f
}