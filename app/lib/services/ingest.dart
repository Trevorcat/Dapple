import 'dart:async';
import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/services.dart';
import 'package:path/path.dart' as p;

/// A file offered by an ingest source. Either [uri] (content://) or [path] is set.
class IncomingFile {
  IncomingFile({required this.id, required this.name, required this.addedMs, this.uri, this.path, this.size = 0});
  final String id;
  final String name;
  final int addedMs;
  final String? uri;
  final String? path;
  final int size;
}

/// How photos arrive on the phone. Camera-specific transport (Sony Creators' App
/// drop, Fujifilm FTP, USB-PTP ...) is a new implementation of this interface;
/// nothing else in the app knows about the camera.
abstract class IngestSource {
  String get id;
  String get label;

  /// Pull-style listing of files added at or after [sinceSec] (unix seconds), oldest first.
  Future<List<IncomingFile>> scan({int limit = 100, int sinceSec = 0});

  /// Push-style notifications; emits whenever [scan] may return something new.
  Stream<void> get changes;
}

const MethodChannel _media = MethodChannel('dapple/media');
const EventChannel _mediaEvents = EventChannel('dapple/media_events');

/// Thin access to the Kotlin side (MediaStore).
class MediaBridge {
  static Future<List<IncomingFile>> queryNew({int limit = 100, int sinceSec = 0}) async {
    final rows = await _media.invokeListMethod<Map>('queryNew', {'limit': limit, 'sinceSec': sinceSec});
    return [
      for (final r in rows ?? const <Map>[])
        IncomingFile(
          id: 'ms:${r['id']}',
          name: r['name'] as String,
          addedMs: (r['addedMs'] as num).toInt(),
          uri: r['uri'] as String,
          path: r['path'] as String?,
          size: (r['size'] as num).toInt(),
        )
    ];
  }

  static Future<String> copyToFile(String uri, String dest) async =>
      (await _media.invokeMethod<String>('copyToFile', {'uri': uri, 'dest': dest}))!;

  /// Inserts the JPEG into MediaStore Pictures/Dapple. Returns the new URI / path.
  /// [replace] first removes this app's earlier gallery item with the same name (regenerated Motion Photos).
  static Future<String> saveToGallery(String path, String name, {bool replace = false}) async =>
      (await _media.invokeMethod<String>('saveToGallery', {'path': path, 'name': name, 'replace': replace}))!;
}

/// Android: MediaStore.Files (images + RAW by extension) with a ContentObserver.
/// Covers Sony Creators' App / Imaging Edge Mobile / Fujifilm XApp, which drop files in DCIM / Pictures.
class MediaStoreIngestSource implements IngestSource {
  @override
  String get id => 'mediastore';
  @override
  String get label => '相册 / 相机 App';

  @override
  Future<List<IncomingFile>> scan({int limit = 100, int sinceSec = 0}) async {
    if (!Platform.isAndroid) return const [];
    return MediaBridge.queryNew(limit: limit, sinceSec: sinceSec);
  }

  @override
  Stream<void> get changes {
    if (!Platform.isAndroid) return const Stream.empty();
    return _mediaEvents.receiveBroadcastStream().map((_) {});
  }
}

/// System file picker (SAF): picked files are copied into [inboxDir] so FFI gets a real path.
class PickerIngestSource implements IngestSource {
  PickerIngestSource(this.inboxDir);
  final Directory inboxDir;

  @override
  String get id => 'picker';
  @override
  String get label => '手动导入';

  @override
  Future<List<IncomingFile>> scan({int limit = 100, int sinceSec = 0}) async => const [];
  @override
  Stream<void> get changes => const Stream.empty();

  /// Shows the picker and returns the imported copies.
  Future<List<IncomingFile>> pick() async {
    final res = await FilePicker.pickFiles(type: FileType.any);
    if (res.isEmpty) return const [];
    await inboxDir.create(recursive: true);
    final out = <IncomingFile>[];
    final now = DateTime.now().millisecondsSinceEpoch;
    for (final f in res) {
      final src = f.path;
      if (src == null) continue;
      final ext = p.extension(f.name).toLowerCase();
      if (!const ['.arw', '.raf', '.dng', '.jpg', '.jpeg'].contains(ext)) continue;
      final dest = p.join(inboxDir.path, '${now + out.length}_${f.name}');
      await File(src).copy(dest);
      out.add(IncomingFile(
          id: 'imp:${now + out.length}_${f.name}',
          name: f.name,
          addedMs: now + out.length,
          path: dest,
          size: await File(dest).length()));
    }
    return out;
  }
}
