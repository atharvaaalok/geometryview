import AppKit
import GeometryEngine

let app = NSApplication.shared
app.setActivationPolicy(.regular)
print("Geometry engine \(String(cString: geo_engine_version()))")
