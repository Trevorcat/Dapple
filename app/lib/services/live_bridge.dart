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

/// 源色彩: how the camera footage is interpreted. Sony tags S-Log3 as plain BT.709, so 自动 looks for Sony's own
/// metadata in the file; 标准 never converts; S-Log3 / HLG force that interpretation.
enum LiveSource {
  auto('自动'),
  standard('标准'),
  slog3('S-Log3'),
  hlg('HLG');

  const LiveSource(this.label);
  final String label;
  int get code => index; // = SwVideo.SRC_* on the platform side
  static LiveSource fromCode(int? c) => c != null && c >= 0 && c < values.length ? values[c] : auto;
}

/// What the clip encoder / decoder actually did (recorded with the Motion Photo).
class ClipInfo {
  ClipInfo(this.map);
  final Map<String, dynamic> map;
  String get videoCodec => '${map['videoCodec'] ?? ''}';
  String get profile => '${map['videoProfile'] ?? ''}';
  int get bitDepth => (map['bitDepth'] as num?)?.toInt() ?? 8;
  String get encoder => '${map['encoder'] ?? ''}';
  String get decoder => '${map['decoder'] ?? ''}';
  bool get hardwareDecode => map['hardwareDecode'] == true;
  String get audioHow => '${map['audioHow'] ?? ''}';

  /// The path the codec ladder took for this clip ("HEVC Main10 · encoder (hardware) | decode … | demoted: …"), empty on old items.
  String get codecPath => '${map['codecSummary'] ?? ''}';

  /// "HEVC Main10 10-bit · AAC" – short, for logs and the item details.
  String get summary {
    final base = '${videoCodec.toUpperCase()} $profile $bitDepth-bit · ${audioHow.isEmpty ? '无音频' : audioHow} · 解码 $decoder${hardwareDecode ? '（硬件）' : '（软件）'}';
    return codecPath.isEmpty ? base : '$base · $codecPath';
  }
}

/// Result of the clip encode.
class TrimResult {
  TrimResult(this.path, this.actualStartMs, this.durationMs, this.frames, this.audio, [this.info]);
  final String path;
  final int actualStartMs;
  final int durationMs;
  final int frames;
  final bool audio;
  final ClipInfo? info;
}

/// Result of the still: `kind` is `ultra-hdr` (SDR base + gain map) or `sdr-jpeg`.
class StillResult {
  StillResult(this.path, this.kind);
  final String path;
  final String kind;
}

/// The platform has no decoder that can decode this video correctly (e.g. H.264 4:2:2 10-bit on a device whose
/// decoders only do 8-bit 4:2:0); it would return false-colour frames, so none are produced.
class UnsupportedVideoException implements Exception {
  UnsupportedVideoException(this.detail);
  final String detail;
  String get userMessage => '这段视频的格式（$detail）在此设备上无法正确解码，无法生成动态照片';
  @override
  String toString() => 'UnsupportedVideo($detail)';
}

/// Android MediaMetadataRetriever / MediaExtractor / MediaMuxer access (Kotlin LiveBridge).
class LiveBridge {
  static Future<List<VideoItem>> listVideos({int limit = 50}) async {
    final rows = await _live.invokeListMethod<Map>('listVideos', {'limit': limit});
    return [for (final r in rows ?? const <Map>[]) VideoItem.fromMap(r)];
  }

  static Future<Map<String, dynamic>> info(String uri) async =>
      ((await _live.invokeMapMethod<String, dynamic>('info', {'uri': uri})) ?? {}).cast<String, dynamic>();


  /// How the platform handles this video: `hardware` (MediaCodec), `software` (FFmpeg fallback for 10-bit / 4:2:2 and
  /// other streams no phone decoder supports) or neither (`decodable == false`).
  static Future<Map<String, dynamic>> probe(String uri) async =>
      ((await _live.invokeMapMethod<String, dynamic>('probe', {'uri': uri})) ?? {}).cast<String, dynamic>();

  /// Probes the video inside a Motion Photo JPEG (same map as [probe]).
  static Future<Map<String, dynamic>> probeEmbedded(String path) async =>
      ((await _live.invokeMapMethod<String, dynamic>('probeEmbedded', {'path': path})) ?? {}).cast<String, dynamic>();
  /// The newest MediaStore video named `<stem>.*`, or null when it is gone.
  static Future<VideoItem?> findVideo(String stem) async {
    final m = await _live.invokeMapMethod<String, dynamic>('findVideo', {'stem': stem});
    return m == null ? null : VideoItem.fromMap(m);
  }
  /// One JPEG per time (ms); entries are null when a frame could not be decoded.
  static Future<List<String?>> thumbs(String uri, List<int> timesMs,
      {int maxEdge = 160, required String destDir, String prefix = 't', LiveSource source = LiveSource.auto}) async {
    final r = await _live.invokeListMethod<dynamic>('thumbs',
        {'uri': uri, 'timesMs': timesMs, 'maxEdge': maxEdge, 'destDir': destDir, 'prefix': prefix, 'source': source.code});
    return [for (final x in r ?? const []) x as String?];
  }

  /// Diagnostics: the codec capability report (declared capabilities, ladders, self-test results) as JSON. `run` first runs the
  /// self-tests that have no result yet on this OS build, `force` repeats all of them.
  static Future<String> codecReport({bool run = false, bool force = false}) async =>
      (await _live.invokeMethod<String>('codecReport', {'run': run, 'force': force})) ?? '';

  /// Encoder / decoder capabilities of this device (`main10Encoder` name or null, `ultraHdr`, `videoLib`).
  static Future<Map<String, dynamic>> capabilities() async =>
      ((await _live.invokeMapMethod<String, dynamic>('capabilities')) ?? {}).cast<String, dynamic>();

  /// The Motion Photo still. [compat] = plain SDR JPEG for sharing targets that need it.
  /// Throws [UnsupportedVideoException] when the video cannot be decoded correctly on this device.
  static Future<StillResult> frame(String uri, int timeMs, String dest,
      {int maxEdge = 0, int quality = 92, LiveSource source = LiveSource.auto, bool compat = false}) async {
    try {
      final m = (await _live.invokeMapMethod<String, dynamic>('frame', {
        'uri': uri, 'timeMs': timeMs, 'dest': dest, 'maxEdge': maxEdge, 'quality': quality, 'source': source.code, 'compat': compat,
      }))!;
      return StillResult(m['path'] as String, '${m['kind']}');
    } on PlatformException catch (e) {
      if (e.code == 'unsupported') {
        throw UnsupportedVideoException((e.message ?? '').replaceFirst('unsupported video: ', ''));
      }
      rethrow;
    }
  }

  /// Decodes and re-encodes the clip: HEVC Main10 when the device can, else H.264 8-bit; [compat] = 8-bit H.264 for sharing.
  static Future<TrimResult> trim(String uri,
      {required int startMs,
      required int durationMs,
      required String dest,
      required bool mute,
      LiveSource source = LiveSource.auto,
      bool compat = false,
      bool film = false}) async {
    final m = (await _live.invokeMapMethod<String, dynamic>('trim', {
      'uri': uri, 'startMs': startMs, 'durationMs': durationMs, 'dest': dest, 'mute': mute, 'source': source.code, 'compat': compat, 'film': film,
    }))!;
    return TrimResult(m['path'] as String, (m['actualStartMs'] as num).toInt(), (m['durationMs'] as num).toInt(),
        (m['frames'] as num).toInt(), m['audio'] == true, ClipInfo(m.cast<String, dynamic>()));
  }
}
