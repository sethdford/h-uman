// Auto-generated from design-tokens/ — do not edit manually
import SwiftUI

/// Design-system tokens for HumanChatUI: colors, spacing, radii, typography,
/// motion, glass, and chart palette.
///
/// Sourced from `design-tokens/` at build time; do not edit manually. Use the
/// nested `Dark` / `Light` enums for color values, choosing based on the
/// SwiftUI `colorScheme` environment.
@available(macOS 14.0, iOS 17.0, *)
public enum HUTokens {
    // MARK: - Colors (Dark)

    /// Dark-mode color palette.
    public enum Dark {
        /// Primary brand accent color (dark mode).
        public static let accent = Color(hex: 0x7AB648)
        /// Primary brand accent color, hover state (dark mode).
        public static let accentHover = Color(hex: 0x8DC63F)
        /// Secondary accent color (dark mode).
        public static let accentSecondary = Color(hex: 0xF59E0B)
        /// Secondary accent color, hover state (dark mode).
        public static let accentSecondaryHover = Color(hex: 0xFBBF24)
        /// Secondary accent color, strong tier (dark mode).
        public static let accentSecondaryStrong = Color(hex: 0xFCD34D)
        /// Secondary accent color, subtle fill tier (dark mode).
        public static let accentSecondarySubtle = Color(red: 0.9608, green: 0.6196, blue: 0.0431, opacity: 0.14)
        /// Secondary accent color used for text-on-surface (dark mode).
        public static let accentSecondaryText = Color(hex: 0xFBBF24)
        /// Primary accent color, strong tier (dark mode).
        public static let accentStrong = Color(hex: 0xA3D46A)
        /// Primary accent color, subtle fill tier (dark mode).
        public static let accentSubtle = Color(red: 0.4784, green: 0.7137, blue: 0.2824, opacity: 0.14)
        /// Tertiary accent color (dark mode).
        public static let accentTertiary = Color(hex: 0x4A6FA5)
        /// Tertiary accent color, hover state (dark mode).
        public static let accentTertiaryHover = Color(hex: 0x7496C4)
        /// Tertiary accent color, strong tier (dark mode).
        public static let accentTertiaryStrong = Color(hex: 0x9FB8D9)
        /// Tertiary accent color, subtle fill tier (dark mode).
        public static let accentTertiarySubtle = Color(red: 0.2902, green: 0.4353, blue: 0.6471, opacity: 0.14)
        /// Tertiary accent color used for text-on-surface (dark mode).
        public static let accentTertiaryText = Color(hex: 0x7496C4)
        /// Primary accent color used for text-on-surface (dark mode).
        public static let accentText = Color(hex: 0x8DC63F)
        /// Modal-backdrop scrim color (dark mode).
        public static let backdropOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.6)
        /// Base background color (dark mode).
        public static let bg = Color(hex: 0x0C0A08)
        /// Elevated background color (dark mode).
        public static let bgElevated = Color(hex: 0x28231E)
        /// Inset background color for sunken regions (dark mode).
        public static let bgInset = Color(hex: 0x141210)
        /// Background color for overlays (dark mode).
        public static let bgOverlay = Color(hex: 0x332D26)
        /// Surface background color (dark mode).
        public static let bgSurface = Color(hex: 0x1E1A16)
        /// Default border color (dark mode).
        public static let border = Color(hex: 0x3E372F)
        /// Subtle border color (dark mode).
        public static let borderSubtle = Color(hex: 0x28231E)
        /// Overlay tint for disabled controls (dark mode).
        public static let disabledOverlay = Color(red: 0.9882, green: 0.9725, blue: 0.949, opacity: 0.04)
        /// Overlay tint shown while a control is being dragged (dark mode).
        public static let draggedOverlay = Color(red: 1, green: 1, blue: 1, opacity: 0.16)
        /// Error color (dark mode).
        public static let error = Color(hex: 0xF97066)
        /// Dimmed error fill for backgrounds (dark mode).
        public static let errorDim = Color(red: 0.9765, green: 0.4392, blue: 0.4, opacity: 0.12)
        /// Overlay tint for focused controls (dark mode).
        public static let focusOverlay = Color(red: 1, green: 1, blue: 1, opacity: 0.12)
        /// Focus ring stroke color (dark mode).
        public static let focusRing = Color(hex: 0x7AB648)
        /// Overlay tint for hovered controls (dark mode).
        public static let hoverOverlay = Color(red: 1, green: 1, blue: 1, opacity: 0.08)
        /// Info color (dark mode).
        public static let info = Color(hex: 0x3B82F6)
        /// Dimmed info fill for backgrounds (dark mode).
        public static let infoDim = Color(red: 0.2314, green: 0.5098, blue: 0.9647, opacity: 0.15)
        /// Link color (dark mode).
        public static let link = Color(hex: 0x8DC63F)
        /// Active-state link color (dark mode).
        public static let linkActive = Color(hex: 0x7AB648)
        /// Hover-state link color (dark mode).
        public static let linkHover = Color(hex: 0xA3D46A)
        /// Visited-state link color (dark mode).
        public static let linkVisited = Color(hex: 0x5A9A30)
        /// Text color rendered on top of `accent` (dark mode).
        public static let onAccent = Color(hex: 0xFFFFFF)
        /// Text color rendered on top of `accentSecondary` (dark mode).
        public static let onAccentSecondary = Color(hex: 0x0C0A08)
        /// Text color rendered on top of `accentTertiary` (dark mode).
        public static let onAccentTertiary = Color(hex: 0xFFFFFF)
        /// Overlay tint for pressed controls (dark mode).
        public static let pressedOverlay = Color(red: 1, green: 1, blue: 1, opacity: 0.12)
        /// Success color (dark mode).
        public static let success = Color(hex: 0x10B981)
        /// Dimmed success fill for backgrounds (dark mode).
        public static let successDim = Color(red: 0.0627, green: 0.7255, blue: 0.5059, opacity: 0.15)
        /// Brightest surface tier (dark mode).
        public static let surfaceBright = Color(hex: 0x42392E)
        /// Surface container, lowest elevation (dark mode).
        public static let surfaceContainer = Color(hex: 0x1F1C19)
        /// Surface container, high elevation (dark mode).
        public static let surfaceContainerHigh = Color(hex: 0x2A2621)
        /// Surface container, highest elevation (dark mode).
        public static let surfaceContainerHighest = Color(hex: 0x35302A)
        /// Dimmest surface tier (dark mode).
        public static let surfaceDim = Color(hex: 0x080604)
        /// Primary text color (dark mode).
        public static let text = Color(hex: 0xDDD8D3)
        /// Faintest text color (dark mode).
        public static let textFaint = Color(hex: 0x56504A)
        /// Muted text color (dark mode).
        public static let textMuted = Color(hex: 0x8A847E)
        /// Secondary text color (dark mode).
        public static let textSecondary = Color(hex: 0x9E9892)
        /// Tertiary text color (dark mode).
        public static let textTertiary = Color(hex: 0x8A847E)
        /// Warning color (dark mode).
        public static let warning = Color(hex: 0xEAB308)
        /// Dimmed warning fill for backgrounds (dark mode).
        public static let warningDim = Color(red: 0.9176, green: 0.702, blue: 0.0314, opacity: 0.15)
    }

    // MARK: - Colors (Light)

    /// Light-mode color palette.
    public enum Light {
        /// Primary brand accent color (light mode).
        public static let accent = Color(hex: 0x7AB648)
        /// Primary brand accent color, hover state (light mode).
        public static let accentHover = Color(hex: 0x5A9A30)
        /// Primary brand accent color, light tier (light mode).
        public static let accentLight = Color(hex: 0xE2F2C8)
        /// Secondary accent color (light mode).
        public static let accentSecondary = Color(hex: 0xD97706)
        /// Secondary accent color, hover state (light mode).
        public static let accentSecondaryHover = Color(hex: 0xB45309)
        /// Secondary accent color, strong tier (light mode).
        public static let accentSecondaryStrong = Color(hex: 0xF59E0B)
        /// Secondary accent color, subtle fill tier (light mode).
        public static let accentSecondarySubtle = Color(red: 0.851, green: 0.4667, blue: 0.0235, opacity: 0.1)
        /// Secondary accent color used for text-on-surface (light mode).
        public static let accentSecondaryText = Color(hex: 0xB45309)
        /// Primary accent color, strong tier (light mode).
        public static let accentStrong = Color(hex: 0x7AB648)
        /// Primary accent color, subtle fill tier (light mode).
        public static let accentSubtle = Color(red: 0.4784, green: 0.7137, blue: 0.2824, opacity: 0.08)
        /// Tertiary accent color (light mode).
        public static let accentTertiary = Color(hex: 0x3D5D8C)
        /// Tertiary accent color, hover state (light mode).
        public static let accentTertiaryHover = Color(hex: 0x324A73)
        /// Tertiary accent color, strong tier (light mode).
        public static let accentTertiaryStrong = Color(hex: 0x4A6FA5)
        /// Tertiary accent color, subtle fill tier (light mode).
        public static let accentTertiarySubtle = Color(red: 0.2392, green: 0.3647, blue: 0.549, opacity: 0.1)
        /// Tertiary accent color used for text-on-surface (light mode).
        public static let accentTertiaryText = Color(hex: 0x324A73)
        /// Primary accent color used for text-on-surface (light mode).
        public static let accentText = Color(hex: 0x3A6A24)
        /// Modal-backdrop scrim color (light mode).
        public static let backdropOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.3)
        /// Base background color (light mode).
        public static let bg = Color(hex: 0xF5F5F3)
        /// Elevated background color (light mode).
        public static let bgElevated = Color(hex: 0xFAFAF8)
        /// Inset background color for sunken regions (light mode).
        public static let bgInset = Color(hex: 0xEDEDEB)
        /// Background color for overlays (light mode).
        public static let bgOverlay = Color(hex: 0xFFFFFF)
        /// Surface background color (light mode).
        public static let bgSurface = Color(hex: 0xFFFFFF)
        /// Warm-tinted background color (light mode).
        public static let bgWarm = Color(hex: 0xFAF8F6)
        /// Default border color (light mode).
        public static let border = Color(hex: 0xDCDAD8)
        /// Subtle border color (light mode).
        public static let borderSubtle = Color(hex: 0xE8E6E3)
        /// Overlay tint for disabled controls (light mode).
        public static let disabledOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.04)
        /// Overlay tint shown while a control is being dragged (light mode).
        public static let draggedOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.14)
        /// Error color (light mode).
        public static let error = Color(hex: 0xCC0000)
        /// Dimmed error fill for backgrounds (light mode).
        public static let errorDim = Color(red: 0.8, green: 0, blue: 0, opacity: 0.08)
        /// Overlay tint for focused controls (light mode).
        public static let focusOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.1)
        /// Focus ring stroke color (light mode).
        public static let focusRing = Color(hex: 0x5A9A30)
        /// Overlay tint for hovered controls (light mode).
        public static let hoverOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.06)
        /// Info color (light mode).
        public static let info = Color(hex: 0x2563EB)
        /// Dimmed info fill for backgrounds (light mode).
        public static let infoDim = Color(red: 0.1451, green: 0.3882, blue: 0.9216, opacity: 0.1)
        /// Link color (light mode).
        public static let link = Color(hex: 0x5A9A30)
        /// Active-state link color (light mode).
        public static let linkActive = Color(hex: 0x7AB648)
        /// Hover-state link color (light mode).
        public static let linkHover = Color(hex: 0x3A6A24)
        /// Visited-state link color (light mode).
        public static let linkVisited = Color(hex: 0x3A6A24)
        /// Text color rendered on top of `accent` (light mode).
        public static let onAccent = Color(hex: 0xFFFFFF)
        /// Text color rendered on top of `accentSecondary` (light mode).
        public static let onAccentSecondary = Color(hex: 0xFFFFFF)
        /// Text color rendered on top of `accentTertiary` (light mode).
        public static let onAccentTertiary = Color(hex: 0xFFFFFF)
        /// Overlay tint for pressed controls (light mode).
        public static let pressedOverlay = Color(red: 0, green: 0, blue: 0, opacity: 0.1)
        /// Success color (light mode).
        public static let success = Color(hex: 0x008000)
        /// Dimmed success fill for backgrounds (light mode).
        public static let successDim = Color(red: 0, green: 0.502, blue: 0, opacity: 0.08)
        /// Brightest surface tier (light mode).
        public static let surfaceBright = Color(hex: 0xFFFFFF)
        /// Surface container, lowest elevation (light mode).
        public static let surfaceContainer = Color(hex: 0xF8F8F6)
        /// Surface container, high elevation (light mode).
        public static let surfaceContainerHigh = Color(hex: 0xF0EEED)
        /// Surface container, highest elevation (light mode).
        public static let surfaceContainerHighest = Color(hex: 0xE8E7E4)
        /// Dimmest surface tier (light mode).
        public static let surfaceDim = Color(hex: 0xE5E4E1)
        /// Primary text color (light mode).
        public static let text = Color(hex: 0x2D2A26)
        /// Faintest text color (light mode).
        public static let textFaint = Color(hex: 0x9A9490)
        /// Muted text color (light mode).
        public static let textMuted = Color(hex: 0x6B655E)
        /// Secondary text color (light mode).
        public static let textSecondary = Color(hex: 0x4A4540)
        /// Tertiary text color (light mode).
        public static let textTertiary = Color(hex: 0x6B655E)
        /// Warning color (light mode).
        public static let warning = Color(hex: 0xCA8A04)
        /// Dimmed warning fill for backgrounds (light mode).
        public static let warningDim = Color(red: 0.7922, green: 0.5412, blue: 0.0157, opacity: 0.1)
    }

    // MARK: - Spacing

    /// 4 pt — extra-small spacing scale.
    public static let spaceXs: CGFloat = 4
    /// 8 pt — small spacing scale.
    public static let spaceSm: CGFloat = 8
    /// 16 pt — medium spacing scale.
    public static let spaceMd: CGFloat = 16
    /// 24 pt — large spacing scale.
    public static let spaceLg: CGFloat = 24
    /// 32 pt — extra-large spacing scale.
    public static let spaceXl: CGFloat = 32
    /// 48 pt — 2× extra-large spacing scale.
    public static let space2xl: CGFloat = 48

    // MARK: - Radius

    /// 4 pt — small corner radius.
    public static let radiusSm: CGFloat = 4
    /// 8 pt — medium corner radius.
    public static let radiusMd: CGFloat = 8
    /// 12 pt — large corner radius.
    public static let radiusLg: CGFloat = 12
    /// 16 pt — extra-large corner radius.
    public static let radiusXl: CGFloat = 16

    // MARK: - Typography

    /// Primary sans-serif font family.
    public static let fontSans = "Avenir"
    /// Primary monospace font family.
    public static let fontMono = "Geist Mono"

    // MARK: - Font sizes

    /// 28 pt — 2× extra-large text size.
    public static let text2Xl: CGFloat = 28
    /// 11 pt — 2× extra-small text size.
    public static let text2Xs: CGFloat = 11
    /// 34 pt — 3× extra-large text size.
    public static let text3Xl: CGFloat = 34
    /// 16 pt — base body-text size.
    public static let textBase: CGFloat = 16
    /// 44 pt — hero text size.
    public static let textHero: CGFloat = 44
    /// 19 pt — large text size.
    public static let textLg: CGFloat = 19
    /// 14 pt — small text size.
    public static let textSm: CGFloat = 14
    /// 24 pt — extra-large text size.
    public static let textXl: CGFloat = 24
    /// 12 pt — extra-small text size.
    public static let textXs: CGFloat = 12

    // MARK: - Font weights

    /// Black font weight (900).
    public static let weightBlack: CGFloat = 900
    /// Bold font weight (700).
    public static let weightBold: CGFloat = 700
    /// Light font weight (300).
    public static let weightLight: CGFloat = 300
    /// Medium font weight (500).
    public static let weightMedium: CGFloat = 500
    /// Normal font weight (400).
    public static let weightNormal: CGFloat = 400
    /// Semibold font weight (600).
    public static let weightSemibold: CGFloat = 600

    // MARK: - Duration

    /// 25 s — ambient motion duration.
    public static let durationAmbient: Double = 25
    /// 30 s — ambient-slow motion duration.
    public static let durationAmbientslow: Double = 30
    /// 0.1 s — fast UI animation duration.
    public static let durationFast: Double = 0.1
    /// 0.05 s — near-instant UI animation duration.
    public static let durationInstant: Double = 0.05
    /// 0.3 s — moderate UI animation duration.
    public static let durationModerate: Double = 0.3
    /// 0.2 s — normal UI animation duration.
    public static let durationNormal: Double = 0.2
    /// 0.35 s — slow UI animation duration.
    public static let durationSlow: Double = 0.35
    /// 0.5 s — slower UI animation duration.
    public static let durationSlower: Double = 0.5
    /// 0.7 s — slowest UI animation duration.
    public static let durationSlowest: Double = 0.7

    // MARK: - Opacity

    /// Opacity applied to disabled controls.
    public static let opacityDisabled: Double = 0.38
    /// Opacity applied to controls being dragged.
    public static let opacityDragged: Double = 0.16
    /// Opacity applied to focus overlays.
    public static let opacityFocus: Double = 0.12
    /// Opacity applied to hover overlays.
    public static let opacityHover: Double = 0.08
    /// Opacity for heavy overlay scrims.
    public static let opacityOverlayHeavy: Double = 0.64
    /// Opacity for light overlay scrims.
    public static let opacityOverlayLight: Double = 0.08
    /// Opacity for medium overlay scrims.
    public static let opacityOverlayMedium: Double = 0.32
    /// Opacity applied to pressed controls.
    public static let opacityPressed: Double = 0.12

    // MARK: - Glass

    /// Ambient glass opacity base.
    public static let glassAmbient: Double = 0.04
    /// Default glass intensity.
    public static let glassIntensity: Double = 0.15
    /// Chat-actions glass background opacity.
    public static let glassChatActionsBgOpacity: Double = 0.9
    /// Chat glass background opacity.
    public static let glassChatBgOpacity: Double = 0.85
    /// Chat glass border opacity.
    public static let glassChatBorderOpacity: Double = 0.08
    /// Chromatic-aberration overlay opacity.
    public static let glassChromaticOpacity: Double = 0.15
    /// Prominent-tier glass background opacity.
    public static let glassProminentBgOpacity: Double = 0.11
    /// Prominent-tier glass border opacity.
    public static let glassProminentBorderOpacity: Double = 0.12
    /// Prominent-tier glass inset opacity.
    public static let glassProminentInsetOpacity: Double = 0.1
    /// Prominent-tier refraction scale.
    public static let glassProminentRefractionScale: Double = 3
    /// Prominent-tier tint opacity.
    public static let glassProminentTintOpacity: Double = 0.028
    /// Standard-tier glass background opacity.
    public static let glassStandardBgOpacity: Double = 0.085
    /// Standard-tier glass border opacity.
    public static let glassStandardBorderOpacity: Double = 0.08
    /// Standard-tier glass inset opacity.
    public static let glassStandardInsetOpacity: Double = 0.06
    /// Standard-tier refraction scale.
    public static let glassStandardRefractionScale: Double = 2
    /// Standard-tier tint opacity.
    public static let glassStandardTintOpacity: Double = 0.018
    /// Subtle-tier glass background opacity.
    public static let glassSubtleBgOpacity: Double = 0.055
    /// Subtle-tier glass border opacity.
    public static let glassSubtleBorderOpacity: Double = 0.05
    /// Subtle-tier glass inset opacity.
    public static let glassSubtleInsetOpacity: Double = 0
    /// Subtle-tier refraction scale.
    public static let glassSubtleRefractionScale: Double = 0
    /// Subtle-tier tint opacity.
    public static let glassSubtleTintOpacity: Double = 0.008
    /// Hover-state specular highlight boost.
    public static let glassHoverSpecularBoost: Double = 1.4
    /// Scale factor applied while a glass surface is pressed.
    public static let glassPressScale: Double = 0.98
    /// Brightness multiplier applied to the backdrop behind glass.
    public static let glassBackdropBrightness: Double = 1.12
    /// Brightness multiplier applied to icons over glass.
    public static let glassIconBoost: Double = 1.2
    /// Brightness multiplier applied to text over glass.
    public static let glassTextBoost: Double = 1.15
    /// Chat-actions glass blur radius (points).
    public static let glassChatActionsBlur: CGFloat = 16
    /// Chat glass blur radius (points).
    public static let glassChatBlur: CGFloat = 24
    /// Chromatic-aberration spread (points).
    public static let glassChromaticSpread: CGFloat = 2
    /// Prominent-tier glass blur radius (points).
    public static let glassProminentBlur: CGFloat = 32
    /// Standard-tier glass blur radius (points).
    public static let glassStandardBlur: CGFloat = 24
    /// Subtle-tier glass blur radius (points).
    public static let glassSubtleBlur: CGFloat = 12
    /// Focus-glow spread radius (points).
    public static let glassFocusGlowSpread: CGFloat = 8
    /// Blur delta applied while a glass surface is pressed (points).
    public static let glassPressBlurDelta: CGFloat = -4

    // MARK: - Chart / Data Visualization

    /// Brand-aligned chart color.
    public static let chartBrand = Color(hex: 0x7AB648)
    /// Categorical chart color 1.
    public static let chartCategorical1 = Color(hex: 0x7AB648)
    /// Categorical chart color 10.
    public static let chartCategorical10 = Color(hex: 0x009BDE)
    /// Categorical chart color 11.
    public static let chartCategorical11 = Color(hex: 0x9D61CC)
    /// Categorical chart color 12.
    public static let chartCategorical12 = Color(hex: 0xC2CD23)
    /// Categorical chart color 13.
    public static let chartCategorical13 = Color(hex: 0xF0D202)
    /// Categorical chart color 14.
    public static let chartCategorical14 = Color(hex: 0xFF6800)
    /// Categorical chart color 15.
    public static let chartCategorical15 = Color(hex: 0xCC0000)
    /// Categorical chart color 16.
    public static let chartCategorical16 = Color(hex: 0x2B89CB)
    /// Categorical chart color 2.
    public static let chartCategorical2 = Color(hex: 0x4A6FA5)
    /// Categorical chart color 3.
    public static let chartCategorical3 = Color(hex: 0xF59E0B)
    /// Categorical chart color 4.
    public static let chartCategorical4 = Color(hex: 0xF97066)
    /// Categorical chart color 5.
    public static let chartCategorical5 = Color(hex: 0x14B8A6)
    /// Categorical chart color 6.
    public static let chartCategorical6 = Color(hex: 0x9FB8D9)
    /// Categorical chart color 7.
    public static let chartCategorical7 = Color(hex: 0xFCD34D)
    /// Categorical chart color 8.
    public static let chartCategorical8 = Color(hex: 0xA3D46A)
    /// Categorical chart color 9.
    public static let chartCategorical9 = Color(hex: 0x00703C)
    /// Diverging-scale negative end color.
    public static let chartDivergingNegative = Color(hex: 0xF97066)
    /// Diverging-scale neutral midpoint color.
    public static let chartDivergingNeutral = Color(hex: 0x8A847E)
    /// Diverging-scale positive end color.
    public static let chartDivergingPositive = Color(hex: 0x7AB648)
    /// Sequential-scale step 100 color.
    public static let chartSequential100 = Color(hex: 0xE2F2C8)
    /// Sequential-scale step 200 color.
    public static let chartSequential200 = Color(hex: 0xC5E59A)
    /// Sequential-scale step 300 color.
    public static let chartSequential300 = Color(hex: 0xA3D46A)
    /// Sequential-scale step 400 color.
    public static let chartSequential400 = Color(hex: 0x8DC63F)
    /// Sequential-scale step 500 color.
    public static let chartSequential500 = Color(hex: 0x7AB648)
    /// Sequential-scale step 600 color.
    public static let chartSequential600 = Color(hex: 0x5A9A30)
    /// Sequential-scale step 700 color.
    public static let chartSequential700 = Color(hex: 0x3A6A24)
    /// Sequential-scale step 800 color.
    public static let chartSequential800 = Color(hex: 0x366320)
    /// Error-status chart color.
    public static let chartStatusError = Color(hex: 0xF97066)
    /// Info-status chart color.
    public static let chartStatusInfo = Color(hex: 0x4A6FA5)
    /// Muted-status chart color.
    public static let chartStatusMuted = Color(hex: 0x8A847E)
    /// Success-status chart color.
    public static let chartStatusSuccess = Color(hex: 0x7AB648)
    /// Warning-status chart color.
    public static let chartStatusWarning = Color(hex: 0xF59E0B)

    // MARK: - Motion (Spring)

    /// Spring animation for micro interactions.
    public static let springMicro = Animation.spring(response: 0.314, dampingFraction: 0.75)
    /// Spring animation for standard interactions.
    public static let springStandard = Animation.spring(response: 0.444, dampingFraction: 0.707)
    /// Spring animation for expressive interactions.
    public static let springExpressive = Animation.spring(response: 0.574, dampingFraction: 0.639)
    /// Spring animation for dramatic interactions.
    public static let springDramatic = Animation.spring(response: 0.702, dampingFraction: 0.559)
    /// Spring animation tuned for tightly-coupled interactive gestures.
    public static let springInteractive = Animation.spring(response: 0.35, dampingFraction: 0.864)

    // MARK: - Haptic Feedback

    /// Haptic feedback styles bridged to `UIFeedbackGenerator` on iOS.
    public enum Haptic {
        /// Light impact.
        case light
        /// Medium impact.
        case medium
        /// Heavy impact.
        case heavy
        /// Success notification.
        case success
        /// Warning notification.
        case warning
        /// Selection-change feedback.
        case selection

        /// Trigger the haptic effect on devices that support it. No-op on
        /// platforms without `UIKit`.
        public func trigger() {
            #if canImport(UIKit)
            switch self {
            case .light:
                UIImpactFeedbackGenerator(style: .light).impactOccurred()
            case .medium:
                UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            case .heavy:
                UIImpactFeedbackGenerator(style: .heavy).impactOccurred()
            case .success:
                UINotificationFeedbackGenerator().notificationOccurred(.success)
            case .warning:
                UINotificationFeedbackGenerator().notificationOccurred(.warning)
            case .selection:
                UISelectionFeedbackGenerator().selectionChanged()
            }
            #endif
        }
    }
}

extension Color {
    init(hex: UInt, alpha: Double = 1) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255,
            opacity: alpha
        )
    }
}
