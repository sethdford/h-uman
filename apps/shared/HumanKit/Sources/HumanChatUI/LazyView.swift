import SwiftUI

/// Defers building content until the view is actually rendered.
/// Use for heavy tab/navigation destinations to avoid initializing until navigated to.
///
/// IMPORTANT: the placeholder branch must still report a stable, non-empty layout
/// shape to SwiftUI. iOS 18's `TabView` lays out its children's accessibility tree
/// during initial render; when a child body collapses to `EmptyView` (the
/// previous `Group { if hasAppeared {} }` form), the iPhone-16 / iOS 18 simulator
/// stops exposing the parent `TabBar` element to XCUITest until a tab is tapped
/// — which the test cannot do because it can't find the tab bar in the first
/// place. A `Color.clear` fills the role of a zero-cost placeholder that keeps
/// the layout shape stable and the accessibility tree well-formed. See
/// `apps/ios/UITests/HumaniOSFleetUITests.swift::launchAndSettle` and the prior
/// CI breakage in run 25736654202 (45s `app.tabBars.firstMatch` timeout).
@available(macOS 14.0, iOS 17.0, *)
public struct LazyView<Content: View>: View {
    let build: () -> Content
    @State private var hasAppeared = false

    /// Wrap a content view so its body is not evaluated until first appearance.
    ///
    /// - Parameter build: Autoclosure that produces the deferred content.
    public init(_ build: @autoclosure @escaping () -> Content) {
        self.build = build
    }

    /// SwiftUI body. Renders `Color.clear` until first appearance, then the
    /// wrapped content thereafter.
    public var body: some View {
        Group {
            if hasAppeared {
                build()
            } else {
                Color.clear
            }
        }
        .onAppear {
            if !hasAppeared {
                hasAppeared = true
            }
        }
    }
}
