// iOS implementation of PlatformServices (docs/ios/platform-interface.md, docs/ios/README.md).
//
// PhotoKit media library (DappleEnginePlugin.swift: list / copy / save / system picker / change notifications), the share sheet (share_plus),
// AVFoundation capture (ios_capture.dart), and graceful stubs for wireless transfer and the video / codec layer (docs/ios/platform-interface.md).
import 'dart:async';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import '../services/capture.dart';
import '../services/ftp_ingest.dart';
import '../services/ingest.dart';
import '../services/media_permission.dart';
import '../services/share_service.dart';
import 'ios_capture.dart';
import 'platform_services.dart';

class IosPlatformServices implements PlatformServices {
  @override
  String get name => 'ios';
  @override
  final MediaLibrary media = IosPhotoLibrary();
  @override
  final CaptureBackend capture = IosCaptureBackend();
  @override
  final FtpBackend wirelessTransfer = NullFtpBackend();
  @override
  final VideoPlatform video = NullVideoPlatform();
  @override
  ShareService get share => ShareService.instance;
  @override
  bool get gpuView => false;
}

const _photoExt = ['.arw', '.raf', '.dng', '.nef', '.cr2', '.cr3', '.orf', '.rw2', '.jpg', '.jpeg', '.heic', '.heif', '.png', '.tif', '.tiff'];

/// PhotoKit through DappleEnginePlugin.swift: `dapple/media` lists / copies / saves, `dapple/photos` asks for access and runs the system picker.
/// Limited access works as full access for the listing (the system only shows the chosen photos); [presentLimitedPicker] widens the selection.
class IosPhotoLibrary implements MediaLibrary {
  final IngestSource _src = MediaStoreIngestSource();
  final MediaPermission _perm = MediaPermission();
  @override
  IngestSource get source => _src;
  @override
  Future<bool> permissionGranted() => _perm.isGranted();
  @override
  Future<bool> requestPermission() => _perm.request();
  Future<String> accessLevel() => MediaPermission.iosStatus();
  Future<void> presentLimitedPicker() => MediaPermission.presentLimitedPicker();
  @override
  Future<String?> copyToFile(IncomingFile f, String destPath) async => f.uri == null ? f.path : MediaBridge.copyToFile(f.uri!, destPath);
  @override
  Future<List<IncomingFile>> pick(Directory inbox) => PickerIngestSource(inbox).pick();
  @override
  Future<bool> saveToLibrary(String path, String name, {bool replace = false}) async {
    try {
      await MediaBridge.saveToGallery(path, name, replace: replace);
      return true;
    } catch (_) {
      return false; // no permission / cancelled: the caller shows its own message
    }
  }
}

/// Documents/Photos as a photo source (files dropped there through the Files app or `simctl`); used by the debug / autotest hooks.
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
