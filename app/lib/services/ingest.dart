import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';

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

  /// HEIC / PNG / TIFF / WebP file -> sRGB JPEG at [dest] (ImageIO on iOS, ImageDecoder on Android).
  static Future<String> toJpeg(String src, String dest) async =>
      (await _media.invokeMethod<String>('toJpeg', {'src': src, 'dest': dest}))!;

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
    if (!Platform.isAndroid && !Platform.isIOS) return const [];
    return MediaBridge.queryNew(limit: limit, sinceSec: sinceSec); // iOS: PhotoKit (DappleEnginePlugin.swift), same maps
  }

  @override
  Stream<void> get changes {
    if (!Platform.isAndroid && !Platform.isIOS) return const Stream.empty();
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

  /// Shows the picker and returns the imported copies (no duplicates, no failures).
  Future<List<IncomingFile>> pick() async => [for (final i in await pickItems()) ?i.file];

  /// Shows the system picker ([source] "photos" = album, "files" = document picker, Android only) and copies the originals into
  /// [inboxDir]. One entry per chosen item, in the order of the native side: a copy, an item that is already in the library
  /// ([known] holds the ids of the library; nothing is copied for those), or a failure. Cancelled = empty list.
  Future<List<PickedItem>> pickItems({String source = 'photos', Iterable<String> known = const []}) async {
    if (!Platform.isAndroid && !Platform.isIOS) return const [];
    await inboxDir.create(recursive: true);
    final rows = await _photos.invokeListMethod<Map>('pick', {'dest': inboxDir.path, 'source': source, 'known': known.toList()});
    final now = DateTime.now().millisecondsSinceEpoch;
    return [for (final r in rows ?? const <Map>[]) PickedItem.fromRow(r.cast<String, dynamic>(), now)];
  }
}

const MethodChannel _photos = MethodChannel('dapple/photos');

/// One item the user chose in the system picker. [file] is the copy in the inbox (null for a duplicate or a failure);
/// [key] is the library id it gets (the scan's id of the same photo when there is one).
class PickedItem {
  PickedItem({required this.key, required this.name, this.dup = false, this.file, this.error});
  final String key;
  final String name;
  final bool dup;
  final IncomingFile? file;

  /// "unsupported" (not a JPEG / RAW the app can develop) or "copy" (reading or copying the original failed).
  final String? error;

  factory PickedItem.fromRow(Map<String, dynamic> r, int nowMs) {
    final key = r['key'] as String? ?? 'imp:${r['name']}';
    final name = r['name'] as String? ?? '照片';
    final path = r['path'] as String?;
    return PickedItem(
      key: key,
      name: name,
      dup: r['dup'] == true,
      error: r['error'] as String?,
      file: path == null ? null : IncomingFile(id: key, name: name, addedMs: nowMs, path: path, size: (r['size'] as num?)?.toInt() ?? 0),
    );
  }
}
