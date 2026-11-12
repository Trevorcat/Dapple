import Foundation

// Referencing one engine symbol makes the linker keep the load command of dapple.framework, so dyld loads it at launch and
// DynamicLibrary.process() sees the C API (dart:ffi); app/lib/native/dapple_bindings.dart falls back to opening it by name.
@_silgen_name("dapple_version") private func dapple_version_c() -> UnsafePointer<CChar>?

public enum DappleEngine {
    public static var version: String { dapple_version_c().map { String(cString: $0) } ?? "" }
}
