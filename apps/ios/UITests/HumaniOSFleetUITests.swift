import XCTest

/// Native iOS UI tests for the CI fleet (`native-apps-fleet.yml`).
/// Aligned with award-tier expectations: primary journeys, accessibility labels, and touch targets.
final class HumaniOSFleetUITests: XCTestCase {
    private var app: XCUIApplication!

    /// iPhone tab bars only show four items plus **More** when there are six `TabView` children; iPad often shows all six.
    private let primaryTabLabels = ["Overview", "Chat", "Memory", "Sessions", "Tools", "Settings"]

    private enum Timeout {
        /// CI simulators (especially SE / cold workers) need headroom beyond default 20s.
        static let launchForeground: TimeInterval = 45
        static let tabBar: TimeInterval = 45
        static let tabItem: TimeInterval = 25
        static let content: TimeInterval = 25
    }

    /// Apple HIG / award-tier minimum touch target (points).
    private let minimumTouchTarget: CGFloat = 44

    override func setUpWithError() throws {
        continueAfterFailure = false
        app = XCUIApplication()
        app.launchArguments.append("-uitestSkipOnboarding")
    }

    override func tearDownWithError() throws {
        app = nil
    }

    // MARK: - Lifecycle helpers

    /// Stable UI-test identifiers on `ContentView` tab items (`tab_overview`, …).
    private func tabAccessibilityIdentifier(for title: String) -> String {
        "tab_" + title.lowercased().replacingOccurrences(of: " ", with: "_")
    }

    /// Prefer the system tab bar; some iOS versions still expose a `tabBars` node even when tab controls are not its direct children.
    private var primaryTabBar: XCUIElement {
        app.tabBars.firstMatch
    }

    /// Resolves a tab control across iOS versions. On iOS 18+ the accessibility tree structure for
    /// SwiftUI TabView differs from earlier versions — tab items may not appear as `buttons` inside
    /// `tabBars`. This method checks multiple strategies in priority order.
    private func tabBarButton(for title: String) -> XCUIElement {
        let id = tabAccessibilityIdentifier(for: title)

        // Strategy 1: Button by accessibility ID (works when .accessibilityIdentifier propagates)
        let byId = app.buttons[id]
        if byId.waitForExistence(timeout: 8) {
            return byId
        }

        // Strategy 2: Tab bar buttons by exact label
        let bar = primaryTabBar
        if bar.exists {
            let barBtn = bar.buttons[title]
            if barBtn.waitForExistence(timeout: 5) {
                return barBtn
            }
            let barFuzzy = bar.buttons
                .matching(NSPredicate(format: "label CONTAINS[c] %@", title))
                .firstMatch
            if barFuzzy.waitForExistence(timeout: 3) {
                return barFuzzy
            }
        }

        // Strategy 3: App-level button by label (iOS 18+ may flatten tab buttons)
        let appExact = app.buttons[title]
        if appExact.waitForExistence(timeout: 5) {
            return appExact
        }

        // Strategy 4: Any element type with matching label/identifier in tab bar
        if bar.exists {
            let anyInBar = bar.descendants(matching: .any)
                .matching(NSPredicate(format: "label == %@ OR identifier == %@", title, id))
                .firstMatch
            if anyInBar.waitForExistence(timeout: 3) {
                return anyInBar
            }
        }

        // Strategy 5: Any descendant anywhere with matching label
        let anyApp = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label == %@ OR identifier == %@", title, id))
            .firstMatch
        if anyApp.waitForExistence(timeout: 5) {
            return anyApp
        }

        // Strategy 6: First child of tab bar for "Overview" (first tab)
        if title == "Overview" && bar.exists {
            let children = bar.children(matching: .any)
            if children.count > 0 {
                return children.element(boundBy: 0)
            }
        }

        return byId
    }

    /// Overflow **More** tab. Prefer the tab-bar control; iOS 18+ may flatten it to app scope.
    private func moreTabButton() -> XCUIElement {
        let bar = primaryTabBar
        if bar.exists {
            let inBar = bar.buttons["More"]
            if inBar.exists { return inBar }
        }
        let byButton = app.buttons["More"]
        if byButton.exists { return byButton }
        return app.descendants(matching: .any)
            .matching(NSPredicate(format: "label == 'More'"))
            .firstMatch
    }

    /// True when the element's frame intersects the primary tab bar (on-bar tab, not overflow list only).
    private func isOnPrimaryTabBar(_ element: XCUIElement) -> Bool {
        let bar = primaryTabBar
        guard bar.exists, element.exists else { return false }
        return element.frame.intersects(bar.frame)
    }

    /// Award-tier touch target: visible frame at least 44×44 pt (or XCTest reports hittable).
    private func elementHasAdequateTouchTarget(_ element: XCUIElement) -> Bool {
        guard element.exists else { return false }
        if element.isHittable { return true }
        let frame = element.frame
        guard frame.width > 0, frame.height > 0 else { return false }
        return frame.width >= minimumTouchTarget && frame.height >= minimumTouchTarget
    }

    /// XCTest often reports UITabBar **More** as `isHittable == false` while the control is tappable; use coordinate tap as fallback.
    private func tapReachable(_ element: XCUIElement, context: String) {
        XCTAssertTrue(element.waitForExistence(timeout: Timeout.tabItem), "\(context) should exist")
        if element.isHittable {
            element.tap()
            return
        }
        XCTAssertTrue(
            elementHasAdequateTouchTarget(element),
            "\(context) should expose a ≥\(Int(minimumTouchTarget))pt touch target (frame=\(element.frame))",
        )
        element.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
    }

    private func assertTouchTargetReachable(_ element: XCUIElement, context: String) {
        XCTAssertTrue(element.waitForExistence(timeout: Timeout.tabItem), "\(context) should exist")
        XCTAssertTrue(
            elementHasAdequateTouchTarget(element),
            "\(context) should be hittable or have a ≥\(Int(minimumTouchTarget))pt frame (isHittable=\(element.isHittable), frame=\(element.frame))",
        )
    }

    /// Checks whether any primary tab chrome is visible, regardless of element type.
    private func anyPrimaryTabChromeVisible() -> Bool {
        let bar = primaryTabBar
        if bar.exists {
            if bar.buttons.count > 0 { return true }
            if bar.children(matching: .any).count > 0 { return true }
        }
        for label in primaryTabLabels {
            let id = tabAccessibilityIdentifier(for: label)
            if app.buttons[id].exists { return true }
            if app.descendants(matching: .any)
                .matching(NSPredicate(format: "label == %@ OR identifier == %@", label, id))
                .firstMatch.exists {
                return true
            }
        }
        return false
    }

    private func launchAndSettle() {
        app.launch()
        XCTAssertTrue(
            app.wait(for: .runningForeground, timeout: Timeout.launchForeground),
            "App should reach foreground (CI simulator startup)",
        )

        // Wait for any sign that the main tab UI has loaded.
        // iOS versions expose the tab bar differently in the accessibility tree.
        let tabBar = primaryTabBar
        let tabBarAppeared = tabBar.waitForExistence(timeout: Timeout.tabBar)

        // Also check for tab buttons by identifier at app level
        let overviewId = tabAccessibilityIdentifier(for: "Overview")
        let overviewById = app.buttons[overviewId]
        let idAppeared = overviewById.waitForExistence(timeout: tabBarAppeared ? 5 : Timeout.tabBar)

        // Also check for any element labeled "Overview" (covers all element types)
        let overviewByLabel = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label == 'Overview' OR identifier == %@", overviewId))
            .firstMatch
        let labelAppeared = overviewByLabel.waitForExistence(timeout: tabBarAppeared || idAppeared ? 3 : Timeout.tabBar)

        XCTAssertTrue(
            tabBarAppeared || idAppeared || labelAppeared,
            "Tab bar or tab accessibility roots should appear after launch",
        )

        let overview = tabBarButton(for: "Overview")
        XCTAssertTrue(
            overview.waitForExistence(timeout: Timeout.tabBar)
                || anyPrimaryTabChromeVisible(),
            "Primary shell should expose Overview (or first tab when labels differ)",
        )
    }

    /// Row for an overflow tab inside the system **More** list.
    private func moreListCell(for label: String) -> XCUIElement {
        let byLabel = NSPredicate(format: "label == %@", label)
        return app.tables.cells.containing(byLabel).firstMatch
    }

    /// Selects a root tab, using **More** when the item is not on the main tab bar (six tabs on iPhone).
    private func selectPrimaryTab(_ label: String) {
        let direct = tabBarButton(for: label)
        if direct.waitForExistence(timeout: 5), isOnPrimaryTabBar(direct), direct.isHittable {
            direct.tap()
            return
        }
        openMoreOverflowMenu()
        tapOverflowTabRow(label)
    }

    /// Asserts the tab is reachable with a proper touch target: main bar button or **More** list row.
    private func assertPrimaryTabHittable(_ label: String) {
        let direct = tabBarButton(for: label)
        if direct.waitForExistence(timeout: 4), isOnPrimaryTabBar(direct) {
            assertTouchTargetReachable(direct, context: "Tab \(label)")
            return
        }
        openMoreOverflowMenu()
        tapOverflowTabRow(label)
        // Return to a stable tab-bar shell so the next overflow check starts from the main bar.
        if label != primaryTabLabels.last {
            selectPrimaryTab("Overview")
        }
    }

    private func openMoreOverflowMenu() {
        let more = moreTabButton()
        assertTouchTargetReachable(more, context: "More tab")
        tapReachable(more, context: "More tab")
    }

    private func tapOverflowTabRow(_ label: String) {
        let cell = moreListCell(for: label)
        if cell.waitForExistence(timeout: Timeout.tabItem) {
            tapReachable(cell, context: "More list row \(label)")
            return
        }
        let alt = app.tables.staticTexts[label].firstMatch
        XCTAssertTrue(alt.waitForExistence(timeout: 8), "Expected \(label) in More tab list")
        tapReachable(alt, context: "More list row \(label)")
    }

    // MARK: - Tests

    func test_launch_shows_tab_bar_with_core_destinations() throws {
        launchAndSettle()
        for label in ["Overview", "Chat", "Memory", "Sessions"] {
            XCTAssertTrue(
                tabBarButton(for: label).waitForExistence(timeout: Timeout.tabItem),
                "Expected tab bar item \(label)",
            )
        }
        let toolsOnBar = tabBarButton(for: "Tools").waitForExistence(timeout: 3)
        let settingsOnBar = tabBarButton(for: "Settings").waitForExistence(timeout: 3)
        if toolsOnBar, settingsOnBar {
            XCTAssertTrue(tabBarButton(for: "Tools").exists)
            XCTAssertTrue(tabBarButton(for: "Settings").exists)
        } else {
            XCTAssertTrue(
                moreTabButton().waitForExistence(timeout: Timeout.tabItem),
                "With six tabs on iPhone, overflow destinations appear under More",
            )
        }
    }

    func test_primary_tabs_are_hittable_award_touch_targets() throws {
        launchAndSettle()
        for label in primaryTabLabels {
            assertPrimaryTabHittable(label)
        }
    }

    func test_overview_shows_recent_activity_section() throws {
        launchAndSettle()
        selectPrimaryTab("Overview")
        XCTAssertTrue(
            app.staticTexts["Recent Activity"].waitForExistence(timeout: Timeout.content),
            "Overview should surface Recent Activity (density / information architecture)",
        )
    }

    func test_tab_journey_reaches_settings_with_labeled_gateway_field() throws {
        launchAndSettle()
        selectPrimaryTab("Settings")
        XCTAssertTrue(
            app.navigationBars["Settings"].waitForExistence(timeout: Timeout.content),
            "Settings navigation title should appear",
        )
        let serverURL = app.textFields["Server URL"]
        let gatewayURL = app.textFields["Gateway URL"]
        XCTAssertTrue(
            serverURL.waitForExistence(timeout: Timeout.content)
                || gatewayURL.waitForExistence(timeout: 8),
            "Settings must expose a labeled gateway field (VoiceOver / ADA alignment)",
        )
    }

    func test_chat_tab_loads_without_crash() throws {
        launchAndSettle()
        selectPrimaryTab("Chat")
        XCTAssertTrue(tabBarButton(for: "Chat").waitForExistence(timeout: Timeout.tabItem))
    }
}
