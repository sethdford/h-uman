package ai.seaclaw.app.ui

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.material3.Typography
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import ai.seaclaw.app.R

private val DarkColorScheme = darkColorScheme(
    primary = SCTokens.Dark.accent,
    onPrimary = Color.White,
    secondary = SCTokens.Dark.accent,
    onSecondary = Color.White,
    tertiary = SCTokens.Dark.accent,
    surface = SCTokens.Dark.bgSurface,
    onSurface = SCTokens.Dark.text,
    surfaceVariant = SCTokens.Dark.bgElevated,
    onSurfaceVariant = SCTokens.Dark.textMuted,
    error = SCTokens.Dark.error,
    onError = Color.White,
    errorContainer = SCTokens.Dark.errorDim,
    onErrorContainer = SCTokens.Dark.error,
    background = SCTokens.Dark.bg,
    onBackground = SCTokens.Dark.text,
    outline = SCTokens.Dark.border,
    outlineVariant = SCTokens.Dark.borderSubtle
)

private val LightColorScheme = lightColorScheme(
    primary = SCTokens.Light.accent,
    onPrimary = Color.White,
    secondary = SCTokens.Light.accent,
    onSecondary = Color.White,
    tertiary = SCTokens.Light.accent,
    surface = SCTokens.Light.bgSurface,
    onSurface = SCTokens.Light.text,
    surfaceVariant = SCTokens.Light.bgElevated,
    onSurfaceVariant = SCTokens.Light.textMuted,
    error = SCTokens.Light.error,
    onError = Color.White,
    errorContainer = SCTokens.Light.errorDim,
    onErrorContainer = SCTokens.Light.error,
    background = SCTokens.Light.bg,
    onBackground = SCTokens.Light.text,
    outline = SCTokens.Light.border,
    outlineVariant = SCTokens.Light.borderSubtle
)

val AvenirFontFamily = FontFamily(
    Font(R.font.avenir_book, FontWeight.Normal),
    Font(R.font.avenir_medium, FontWeight.Medium),
    Font(R.font.avenir_heavy, FontWeight.Bold),
    Font(R.font.avenir_black, FontWeight.Black)
)

val AvenirTypography = Typography(
    displayLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 57.sp,
        lineHeight = 64.sp,
        letterSpacing = (-0.25).sp
    ),
    displayMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 45.sp,
        lineHeight = 52.sp
    ),
    displaySmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 36.sp,
        lineHeight = 44.sp
    ),
    headlineLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 32.sp,
        lineHeight = 40.sp
    ),
    headlineMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 28.sp,
        lineHeight = 36.sp
    ),
    headlineSmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 24.sp,
        lineHeight = 32.sp
    ),
    titleLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 22.sp,
        lineHeight = 28.sp
    ),
    titleMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = 16.sp,
        lineHeight = 24.sp,
        letterSpacing = 0.15.sp
    ),
    titleSmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = 14.sp,
        lineHeight = 20.sp,
        letterSpacing = 0.1.sp
    ),
    bodyLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 16.sp,
        lineHeight = 24.sp,
        letterSpacing = 0.5.sp
    ),
    bodyMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 14.sp,
        lineHeight = 20.sp,
        letterSpacing = 0.25.sp
    ),
    bodySmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = 12.sp,
        lineHeight = 16.sp,
        letterSpacing = 0.4.sp
    ),
    labelLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = 14.sp,
        lineHeight = 20.sp,
        letterSpacing = 0.1.sp
    ),
    labelMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = 12.sp,
        lineHeight = 16.sp,
        letterSpacing = 0.5.sp
    ),
    labelSmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = 11.sp,
        lineHeight = 16.sp,
        letterSpacing = 0.5.sp
    )
)

@Composable
fun SeaClawTheme(
    content: @Composable () -> Unit
) {
    val darkTheme = isSystemInDarkTheme()

    val colorScheme = if (darkTheme) DarkColorScheme else LightColorScheme

    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = colorScheme.background.toArgb()
            WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = !darkTheme
        }
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = AvenirTypography,
        content = content
    )
}
