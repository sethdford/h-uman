package ai.human.app

import android.content.Intent
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.and
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.AndroidComposeTestRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.rules.ActivityScenarioRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Instrumented journeys aligned with Google Play “Best app” / accessibility expectations:
 * bottom-nav coverage, settings gateway heading, and stable content descriptions.
 */
@RunWith(AndroidJUnit4::class)
class NativeFleetAwardTierTest {
    private val launchIntent: Intent =
        Intent(
            ApplicationProvider.getApplicationContext(),
            MainActivity::class.java,
        ).putExtra(EXTRA_SKIP_ONBOARDING_FOR_TEST, true)

    // NOT a @Rule of its own: AndroidComposeTestRule applies the rule it wraps, so
    // registering it here too launched MainActivity twice per test (two
    // "Displayed ai.human.app/.MainActivity" lines in every logcat), and the
    // Compose assertions ran against a covered instance ("is not displayed",
    // "Failed to inject touch input").
    private val activityRule = ActivityScenarioRule<MainActivity>(launchIntent)

    @get:Rule
    val composeRule: AndroidComposeTestRule<ActivityScenarioRule<MainActivity>, MainActivity> =
        AndroidComposeTestRule(activityRule) { rule ->
            var activity: MainActivity? = null
            rule.scenario.onActivity { activity = it }
            requireNotNull(activity)
        }

    /**
     * A bottom-nav destination as accessibility services see it: the merged Tab node whose
     * text is [label]. Not `onNodeWithContentDescription(label)`: Material3's
     * NavigationBarItem wraps the icon in `clearAndSetSemantics {}` whenever a label is
     * shown, so the icon's contentDescription exists only in the unmerged tree — the finder
     * matched nothing, which `assertIsDisplayed` reports as "is not displayed" and
     * `performClick` as "Failed to inject touch input".
     */
    private fun navTab(label: String): SemanticsNodeInteraction =
        composeRule.onNode(
            hasText(label) and SemanticsMatcher.expectValue(SemanticsProperties.Role, Role.Tab),
        )

    @Test
    fun bottom_nav_destinations_exist() {
        for (label in
            listOf(
                "Overview",
                "Chat",
                "Memory",
                "Sessions",
                "Tools",
                "Settings",
            )) {
            navTab(label).assertIsDisplayed()
        }
    }

    @Test
    fun overview_shows_welcome_heading() {
        navTab("Overview").performClick()
        composeRule.onNodeWithContentDescription("Welcome back").assertIsDisplayed()
    }

    @Test
    fun journey_visits_each_tab_then_opens_gateway_settings() {
        for (label in
            listOf(
                "Overview",
                "Chat",
                "Memory",
                "Sessions",
                "Tools",
                "Settings",
            )) {
            navTab(label).performClick()
        }
        composeRule.onNodeWithContentDescription("Gateway settings").assertIsDisplayed()
    }
}
