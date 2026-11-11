import 'package:flutter/services.dart';

const MethodChannel _live = MethodChannel('dapple/live');

/// A video from MediaStore.
class VideoItem {
  VideoItem({
    required this.id,
    required this.uri,
    required this.name,
    required this.durationMs,
    required this.size,
    required this.addedMs,
    this.width = 0,
    this.height = 0,
  });
  final String id;
  final String uri;
  final String name;
  final int durationMs;
  final int size;
  final int addedMs;
  final int width;
  final int height;

  double get seconds => durationMs / 1000.0;
  String get stem {
    final i = name.lastIndexOf('.');
    return i > 0 ? name.substring(0, i) : name;
  }

  factory VideoItem.fromMap(Map m) => VideoItem(
        id: '${m['id']}',
        uri: m['uri'] as String,
        name: (m['name'] as String?) ?? 'video',
        durationMs: (m['durationMs'] as num).toInt(),
        size: (m['size'] as num?)?.toInt() ?? 0,
        addedMs: (m['addedMs'] as num?)?.toInt() ?? 0,
        width: (m['width'] as num?)?.toInt() ?? 0,
        height: (m['height'] as num?)?.toInt() ?? 0,
      );
}

/// Result of the platform trim.
class TrimResult {
  TrimResult(this.path, this.actualStartMs, this.durationMs, this.frames, this.audio);
  final String path;
  final int actualStartMs;
  final int durationMs;
  final int frames;
  final bool audio;
}

/// Android MediaMetadataRetriever / MediaExtractor / MediaMuxer access (Kotlin LiveBridge).
class LiveBridge {
  static Future<List<VideoItem>> listVideos({int limit = 50}) async {
    final rows = await _live.invokeListMethod<Map>('listVideos', {'limit': limit});
    return [for (final r in rows ?? const <Map>[]) VideoItem.fromMap(r)];
  }

  static Future<Map<String, dynamic>> info(String uri) async =>
      ((await _live.invokeMapMethod<String, dynamic>('info', {'uri': uri})) ?? {}).cast<String, dynamic>();

  /// One JPEG per time (ms); entries are null when a frame could not be decoded.
  static Future<List<String?>> thumbs(String uri, List<int> timesMs,
      {int maxEdge = 160, required String destDir, String prefix = 't'}) async {
    final r = await _live.invokeListMethod<dynamic>(
        'thumbs', {'uri': uri, 'timesMs': timesMs, 'maxEdge': maxEdge, 'destDir': destDir, 'prefix': prefix});
    return [for (final x in r ?? const []) x as String?];
  }

  static Future<String> frame(String uri, int timeMs, String dest, {int maxEdge = 0, int quality = 92}) async =>
      (await _live.invokeMethod<String>('frame', {'uri': uri, 'timeMs': timeMs, 'dest': dest, 'maxEdge': maxEdge, 'quality': quality}))!;

  static Future<TrimResult> trim(String uri, {required int startMs, required int durationMs, required String dest, required bool mute}) async {
    final m = (await _live.invokeMapMethod<String, dynamic>(
        'trim', {'uri': uri, 'startMs': startMs, 'durationMs': durationMs, 'dest': dest, 'mute': mute}))!;
    return TrimResult(m['path'] as String, (m['actualStartMs'] as num).toInt(), (m['durationMs'] as num).toInt(),
        (m['frames'] as num).toInt(), m['audio'] == true);
  }
}
