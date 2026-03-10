package ai.human.app.ui

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
import ai.human.app.R

private val DarkColorScheme = darkColorScheme(
    primary = HUTokens.Dark.accent,
    onPrimary = Color.White,
    secondary = HUTokens.Dark.accent,
    onSecondary = Color.White,
    tertiary = HUTokens.Dark.accent,
    surface = HUTokens.Dark.bgSurface,
    onSurface = HUTokens.Dark.text,
    surfaceVariant = HUTokens.Dark.bgElevated,
    onSurfaceVariant = HUTokens.Dark.textMuted,
    error = HUTokens.Dark.error,
    onError = Color.White,
    errorContainer = HUTokens.Dark.errorDim,
    onErrorContainer = HUTokens.Dark.error,
    background = HUTokens.Dark.bg,
    onBackground = HUTokens.Dark.text,
    outline = HUTokens.Dark.border,
    outlineVariant = HUTokens.Dark.borderSubtle
)

private val LightColorScheme = lightColorScheme(
    primary = HUTokens.Light.accent,
    onPrimary = Color.White,
    secondary = HUTokens.Light.accent,
    onSecondary = Color.White,
    tertiary = HUTokens.Light.accent,
    surface = HUTokens.Light.bgSurface,
    onSurface = HUTokens.Light.text,
    surfaceVariant = HUTokens.Light.bgElevated,
    onSurfaceVariant = HUTokens.Light.textMuted,
    error = HUTokens.Light.error,
    onError = Color.White,
    errorContainer = HUTokens.Light.errorDim,
    onErrorContainer = HUTokens.Light.error,
    background = HUTokens.Light.bg,
    onBackground = HUTokens.Light.text,
    outline = HUTokens.Light.border,
    outlineVariant = HUTokens.Light.borderSubtle
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
fun HumanTheme(
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
