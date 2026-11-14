// Method "pickVideoFile" (channel dapple/photos): the system file picker (UIDocumentPickerViewController, runs out of process, needs no permission)
// for one video, for the Live Photo tool. The file is copied into the app's temporary folder (<tmp>/dapple-video-pick/<name>) and its path is returned;
// null when the user cancels. Errors: busy (a picker is already open), nopicker (no view controller), copy.
import Flutter
import UIKit
import UniformTypeIdentifiers

final class DappleVideoPicker: NSObject, UIDocumentPickerDelegate {
    static let shared = DappleVideoPicker()
    private var pending: FlutterResult?

    func pick(from vc: UIViewController?, result: @escaping FlutterResult) {
        guard pending == nil else { return result(FlutterError(code: "busy", message: "a picker is already open", details: nil)) }
        guard let vc = vc else { return result(FlutterError(code: "nopicker", message: "no view controller", details: nil)) }
        pending = result
        let p = UIDocumentPickerViewController(forOpeningContentTypes: [.movie, .video], asCopy: true)  // asCopy: the system copies into our inbox, no scoped access to manage
        p.allowsMultipleSelection = false
        p.delegate = self
        vc.present(p, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let src = urls.first else { return finish(nil) }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true).appendingPathComponent("dapple-video-pick", isDirectory: true)
        do {
            try? FileManager.default.removeItem(at: dir)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let dst = dir.appendingPathComponent(src.lastPathComponent)
            try FileManager.default.copyItem(at: src, to: dst)
            try? FileManager.default.removeItem(at: src)  // the picker's own copy
            finish(dst.path)
        } catch {
            let r = pending
            pending = nil
            r?(FlutterError(code: "copy", message: error.localizedDescription, details: nil))
        }
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { finish(nil) }

    private func finish(_ path: String?) {
        let r = pending
        pending = nil
        r?(path)
    }
}
