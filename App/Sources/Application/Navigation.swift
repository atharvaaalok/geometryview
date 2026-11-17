import Foundation

/// Where the window has been, for Back and Forward: folders shown in the gallery and the files open
/// in the viewer. The web viewer's navigation.js keeps the same rules; both are checked against
/// Tests/data/navigation.json (`--navigation-check` here).
struct Navigation {
    enum Place: Equatable {
        /// A folder of a connected root, by its path from the root ("" for the root).
        case folder(root: URL, path: String)
        /// The files open in the viewer.
        case files([URL])

        var isFiles: Bool {
            if case .files = self {
                return true
            }
            return false
        }
    }

    /// What a folder showed when it was left, to show again on coming back to it.
    struct Shown {
        var view: UInt32
        var scroll: CGFloat
        /// The file selected, by its path from the root.
        var selected: String?
        var search: String
    }

    struct Step {
        var place: Place
        var shown: Shown?
    }

    /// How a visit moved: to what shows already, to the step beside, or to a new step.
    enum Move: String {
        case same, back, forward, new
    }

    static let limit = 50

    private(set) var steps: [Step] = []
    private(set) var at = -1

    var current: Step? { steps.indices.contains(at) ? steps[at] : nil }
    /// The step Back goes to.
    var behind: Step? { at > 0 ? steps[at - 1] : nil }
    var canGoBack: Bool { at > 0 }
    var canGoForward: Bool { at + 1 < steps.count }

    /// Shows `place`. The step beside, behind or ahead, is moved to rather than repeated; anywhere
    /// else is a new step, which drops the steps ahead.
    @discardableResult
    mutating func visit(_ place: Place) -> Move {
        if current?.place == place {
            return .same
        }
        if at > 0, steps[at - 1].place == place {
            at -= 1
            return .back
        }
        if at + 1 < steps.count, steps[at + 1].place == place {
            at += 1
            return .forward
        }
        steps.removeSubrange((at + 1)...)
        steps.append(Step(place: place))
        if steps.count > Self.limit {
            steps.removeFirst(steps.count - Self.limit)
        }
        at = steps.count - 1
        return .new
    }

    /// A file added to the view: the step that holds files now holds `place`; from a folder, a step.
    @discardableResult
    mutating func add(_ place: Place) -> Move {
        if let current, current.place.isFiles {
            steps[at].place = place
            return .same
        }
        return visit(place)
    }

    mutating func back() -> Step? {
        guard canGoBack else {
            return nil
        }
        at -= 1
        return steps[at]
    }

    mutating func forward() -> Step? {
        guard canGoForward else {
            return nil
        }
        at += 1
        return steps[at]
    }

    /// What the folder at `place` showed, kept for when it shows again.
    mutating func keep(_ shown: Shown, for place: Place) {
        if current?.place == place {
            steps[at].shown = shown
        }
    }
}

/// `--navigation-check < Tests/data/navigation.json`: runs the cases the web viewer shares, prints
/// each, and exits 0 if all pass. Needs no window: it runs before the app starts.
enum NavigationCheck {
    static var isRequested: Bool { CommandLine.arguments.contains("--navigation-check") }

    static func run() -> Int32 {
        let data = FileHandle.standardInput.readDataToEndOfFile()
        guard let shared = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let cases = shared["cases"] as? [[String: Any]]
        else {
            print("navigation-check: no cases on standard input")
            return 2
        }
        var failures = 0
        if shared["limit"] as? Int != Navigation.limit {
            print(
                "navigation-check: the limit is \(Navigation.limit), the web's is \(shared["limit"] ?? "?")"
            )
            failures += 1
        }
        for test in cases {
            let name = test["name"] as? String ?? "?"
            var failed: String?
            var navigation = Navigation()
            for (index, event) in (test["events"] as? [[String: Any]] ?? []).enumerated() {
                failed = check(event, on: &navigation).map { "event \(index): \($0)" }
                if failed != nil {
                    break
                }
            }
            print(
                "navigation-check: \(failed == nil ? "ok" : "FAILED") \(name)\(failed.map { " — \($0)" } ?? "")"
            )
            failures += failed == nil ? 0 : 1
        }
        print("navigation-check: \(cases.count - failures) of \(cases.count) passed")
        return failures == 0 ? 0 : 1
    }

    /// Runs one event, and says what differs from what it expects, if anything.
    private static func check(_ event: [String: Any], on navigation: inout Navigation) -> String? {
        let action = event["do"] as? String ?? ""
        if action == "visitMany" {
            let prefix = event["prefix"] as? String ?? ""
            for n in 0..<(event["count"] as? Int ?? 0) {
                navigation.visit(place("\(prefix)\(n)"))
            }
            let first = navigation.steps.first.map { key($0.place) }
            if navigation.steps.count != event["length"] as? Int
                || first != event["first"] as? String
                || navigation.at != event["at"] as? Int
            {
                return
                    "\(navigation.steps.count) steps from \(first ?? "nothing"), at \(navigation.at)"
            }
            return nil
        }
        let expected = event["place"] as? String
        switch action {
        case "visit", "add":
            let move =
                action == "visit"
                ? navigation.visit(place(expected ?? "")) : navigation.add(place(expected ?? ""))
            if move.rawValue != event["move"] as? String {
                return "\(action) moved \(move.rawValue)"
            }
        case "back", "forward":
            let step = action == "back" ? navigation.back() : navigation.forward()
            if step.map({ key($0.place) }) != expected {
                return "\(action) went to \(step.map { key($0.place) } ?? "nothing")"
            }
        default:
            return "unknown event \(action)"
        }
        let steps = navigation.steps.map { key($0.place) }
        if steps != event["steps"] as? [String] ?? [] || navigation.at != event["at"] as? Int {
            return "steps \(steps), at \(navigation.at)"
        }
        return nil
    }

    /// "folder:<root>|<path>" or "files:<path>,<path>", as a place.
    private static func place(_ text: String) -> Navigation.Place {
        let colon = text.firstIndex(of: ":") ?? text.startIndex
        let rest = String(text[text.index(after: colon)...])
        if text.hasPrefix("folder:") {
            let parts = rest.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false)
            return .folder(
                root: URL(fileURLWithPath: String(parts[0])),
                path: parts.count > 1 ? String(parts[1]) : ""
            )
        }
        return .files(rest.split(separator: ",").map { URL(fileURLWithPath: String($0)) })
    }

    private static func key(_ place: Navigation.Place) -> String {
        switch place {
        case .folder(let root, let path):
            return "folder:\(root.path)|\(path)"
        case .files(let urls):
            return "files:\(urls.map(\.path).joined(separator: ","))"
        }
    }
}
