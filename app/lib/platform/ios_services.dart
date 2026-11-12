// iOS implementation of PlatformServices (docs/ios/platform-interface.md, docs/ios/README.md).
//
// Working today: a file based media library (the app's Documents/Photos folder, which is also visible in the Files app under
// "On My iPhone / Dapple": copy RAW / JPEG files there, or push them with `simctl` on the simulator) and the system share sheet.
// Stubs that fail gracefully: capture, wireless transfer, video / codec layer, saving to the Photos library (Swift side planned:
// AVFoundation, Network.framework, VideoToolbox, PhotoKit).
import 'dart:async';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../services/capture.dart';
import '../services/ftp_ingest.dart';
import '../services/ingest.dart';
import '../services/share_service.dart';
import 'platform_services.dart';

class IosPlatformServices implements PlatformServices {
  @override
  String get name => 'ios';
  @override
  final MediaLibrary media = IosDocumentsLibrary();
  @override
  final CaptureBackend capture = NullCaptureBackend();
  @override
  final FtpBackend wirelessTransfer = NullFtpBackend();
  @override
  final VideoPlatform video = NullVideoPlatform();
  @override
  ShareService get share => ShareService.instance;
  @override
  bool get gpuView => false;
}

const _photoExt = ['.arw', '.raf', '.dng', '.nef', '.cr2', '.cr3', '.orf', '.rw2', '.jpg', '.jpeg', '.heic'];

/// Documents/Photos as the photo source. Files are read in place (the engine takes paths), nothing is copied.
class IosDocumentsLibrary implements MediaLibrary {
  final IosDocumentsSource _src = IosDocumentsSource();
  @override
  IngestSource get source => _src;
  @override
  Future<bool> permissionGranted() async => true; // files in the app's own sandbox need no permission
  @override
  Future<bool> requestPermission() async => true;
  @override
  Future<String?> copyToFile(IncomingFile f, String destPath) async => f.path;
  @override
  Future<bool> saveToLibrary(String path, String name, {bool replace = false}) async => false; // PhotoKit: planned
}

class IosDocumentsSource implements IngestSource {
  @override
  String get id => 'documents';
  @override
  String get label => 'Documents/Photos';

  static Future<Directory> folder() async {
    final d = Directory(p.join((await getApplicationDocumentsDirectory()).path, 'Photos'));
    if (!d.existsSync()) await d.create(recursive: true);
    return d;
  }

  @override
  Future<List<IncomingFile>> scan({int limit = 100, int sinceSec = 0}) async {
    try {
      final out = <IncomingFile>[];
      for (final e in (await folder()).listSync(followLinks: false)) {
        if (e is! File || !_photoExt.contains(p.extension(e.path).toLowerCase())) continue;
        final st = e.statSync();
        final sec = st.modified.millisecondsSinceEpoch ~/ 1000;
        if (sec < sinceSec) continue;
        out.add(IncomingFile(id: 'doc:${p.basename(e.path)}', name: p.basename(e.path), addedMs: st.modified.millisecondsSinceEpoch, path: e.path, size: st.size));
      }
      out.sort((a, b) => a.addedMs.compareTo(b.addedMs));
      return out.length > limit ? out.sublist(0, limit) : out;
    } catch (_) {
      return const [];
    }
  }

  @override
  Stream<void> get changes => const Stream.empty();
}
