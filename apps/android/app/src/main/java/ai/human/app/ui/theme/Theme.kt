package ai.human.app.ui.theme

import android.os.Build
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import ai.human.app.R
import ai.human.app.ui.HUTokens

val AvenirFontFamily = FontFamily(
    Font(R.font.avenir_book, FontWeight.Normal),
    Font(R.font.avenir_medium, FontWeight.Medium),
    Font(R.font.avenir_heavy, FontWeight.Bold),
    Font(R.font.avenir_black, FontWeight.Black),
)

private val AvenirTypography = Typography(
    displayLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Bold,
        fontSize = HUTokens.textXl,
        lineHeight = (HUTokens.textXl.value * HUTokens.leadingTight).sp,
        letterSpacing = (HUTokens.trackingHero * HUTokens.textXl.value).sp,
    ),
    displayMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Bold,
        fontSize = HUTokens.textLg,
        lineHeight = (HUTokens.textLg.value * HUTokens.leadingTight).sp,
        letterSpacing = (HUTokens.trackingXl * HUTokens.textLg.value).sp,
    ),
    displaySmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Bold,
        fontSize = HUTokens.textLg,
        lineHeight = (HUTokens.textLg.value * HUTokens.leadingSnug).sp,
        letterSpacing = HUTokens.trackingBase.sp,
    ),
    headlineLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Bold,
        fontSize = HUTokens.textXl,
        lineHeight = (HUTokens.textXl.value * HUTokens.leadingSnug).sp,
        letterSpacing = (HUTokens.trackingXl * HUTokens.textXl.value).sp,
    ),
    headlineMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textLg,
        lineHeight = (HUTokens.textLg.value * HUTokens.leadingSnug).sp,
        letterSpacing = (HUTokens.trackingLg * HUTokens.textLg.value).sp,
    ),
    headlineSmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textLg,
        lineHeight = (HUTokens.textLg.value * HUTokens.leadingNormal).sp,
        letterSpacing = HUTokens.trackingBase.sp,
    ),
    titleLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textLg,
        lineHeight = (HUTokens.textLg.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.trackingSm * HUTokens.textLg.value).sp,
    ),
    titleMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textBase,
        lineHeight = (HUTokens.textBase.value * HUTokens.leadingSnug).sp,
        letterSpacing = (HUTokens.trackingXs * HUTokens.textBase.value).sp,
    ),
    titleSmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textBase,
        lineHeight = (HUTokens.textBase.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.trackingXs * HUTokens.textBase.value).sp,
    ),
    bodyLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = HUTokens.textLg,
        lineHeight = (HUTokens.textLg.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.trackingBase * HUTokens.textLg.value).sp,
    ),
    bodyMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = HUTokens.textBase,
        lineHeight = (HUTokens.textBase.value * HUTokens.leadingNormal).sp,
        letterSpacing = HUTokens.trackingBase.sp,
    ),
    bodySmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Normal,
        fontSize = HUTokens.textSm,
        lineHeight = (HUTokens.textSm.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.trackingXs * HUTokens.textSm.value).sp,
    ),
    labelLarge = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textBase,
        lineHeight = (HUTokens.textBase.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.trackingXs * HUTokens.textBase.value).sp,
    ),
    labelMedium = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textSm,
        lineHeight = (HUTokens.textSm.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.trackingXs * HUTokens.textSm.value).sp,
    ),
    labelSmall = TextStyle(
        fontFamily = AvenirFontFamily,
        fontWeight = FontWeight.Medium,
        fontSize = HUTokens.textXs,
        lineHeight = (HUTokens.textXs.value * HUTokens.leadingNormal).sp,
        letterSpacing = (HUTokens.tracking2xs * HUTokens.textXs.value).sp,
    ),
)

@Composable
fun HumanTheme(
    darkTheme: Boolean = androidx.compose.foundation.isSystemInDarkTheme(),
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            if (darkTheme) dynamicDarkColorScheme(context)
            else dynamicLightColorScheme(context)
        }
        darkTheme -> {
        val t = HUTokens.Dark
        darkColorScheme(
            primary = t.accent,
            onPrimary = t.onAccent,
            primaryContainer = t.surfaceContainer,
            secondary = t.accentSecondary,
            onSecondary = t.onAccentSecondary,
            tertiary = t.accentTertiary,
            onTertiary = t.onAccentTertiary,
            background = t.bg,
            onBackground = t.text,
            surface = t.bgSurface,
            onSurface = t.text,
            surfaceVariant = t.surfaceContainer,
            surfaceContainer = t.surfaceContainer,
            surfaceContainerLow = t.surfaceDim,
            surfaceContainerHigh = t.surfaceContainerHigh,
            surfaceContainerHighest = t.surfaceContainerHighest,
            error = t.error,
            onError = t.bg,
            outline = t.border,
            outlineVariant = t.borderSubtle,
        )
        }
        else -> {
        val t = HUTokens.Light
        lightColorScheme(
            primary = t.accent,
            onPrimary = t.onAccent,
            primaryContainer = t.surfaceContainer,
            secondary = t.accentSecondary,
            onSecondary = t.onAccentSecondary,
            tertiary = t.accentTertiary,
            onTertiary = t.onAccentTertiary,
            background = t.bg,
            onBackground = t.text,
            surface = t.bgSurface,
            onSurface = t.text,
            surfaceVariant = t.surfaceContainer,
            surfaceContainer = t.surfaceContainer,
            surfaceContainerLow = t.surfaceDim,
            surfaceContainerHigh = t.surfaceContainerHigh,
            surfaceContainerHighest = t.surfaceContainerHighest,
            error = t.error,
            onError = t.bg,
            outline = t.border,
            outlineVariant = t.borderSubtle,
        )
        }
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = AvenirTypography,
        content = content,
    )
}
