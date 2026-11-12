import 'model/params.dart';

/// Per-photo lifecycle (requirements 3.1).
enum PhotoStatus {
  /// Still being received (progress 0..1). Not openable, not selectable.
  arriving,

  /// Waiting for the develop queue (shows the in-camera preview, "预览").
  pending,
  developing,
  done,

  /// Transfer interrupted ("传输中断" + retry).
  interrupted,

  /// Develop failed locally; shown like interrupted with a retry.
  failed,
}

/// Composite kinds of the "昨天" group (requirements 4.2.4).
enum PhotoKind { photo, hdr, pano, live, video, bracket, frame }

/// A photo known to the app (MediaStore, system picker, a future FTP source, or the demo set).
class Photo {
  Photo({
    required this.id,
    required this.name,
    required this.addedMs,
    this.uri,
    this.path,
    this.camera,
    this.status = PhotoStatus.pending,
    this.previewPath,
    this.edit = EditState.empty,
    this.correction,
    this.error,
    this.exported = false,
    this.progress = 0,
    this.flag = false,
    this.ratio,
    this.pxW,
    this.pxH,
    this.exif,
    this.demo,
    this.place,
    this.kind = PhotoKind.photo,
    this.badge,
    this.nearest,
    this.takenMs,
    this.pairedJpeg,
    this.liveSrc,
    this.liveStartMs,
    this.liveCoverMs,
    this.liveMute = false,
    this.liveGen = 0,
    this.liveSource = 0,
    this.liveInfo,
  });

  final String id;
  final String name;
  final int addedMs;

  /// content:// URI (MediaStore) when the file is not directly readable.
  final String? uri;

  /// Real file path (imported copy, or MediaStore DATA path when readable).
  String? path;
  String? camera;
  PhotoStatus status;
  String? previewPath;
  EditState edit;

  /// Result of dapple_session_auto_json()["correction"] (legacy EditParams keys).
  Map<String, dynamic>? correction;
  String? error;
  bool exported;

  /// 0..1 while [PhotoStatus.arriving].
  double progress;

  /// Engine "建议检查" flag (amber dot).
  bool flag;

  /// width / height; null until known.
  double? ratio;

  /// Original pixel size (long side is the sensor / file resolution); null until probed.
  int? pxW, pxH;
  String? exif;

  /// Index into `demoPhotos` (tests / screenshots only).
  int? demo;

  /// Location label for the group title, e.g. 外滩.
  String? place;
  PhotoKind kind;

  /// Corner badge text for composite-group items (EV, 1/4, HDR ...).
  String? badge;

  /// Nearest tone class from the engine ("mid-long"), cached.
  String? nearest;

  /// Capture time (EXIF) in ms since epoch; null until the file was probed.
  int? takenMs;

  /// RAW+JPEG pair: the camera JPEG that arrived with this RAW (kept, never listed on its own).
  String? pairedJpeg;

  /// Motion Photo (kind == live) provenance, so it can be regenerated: source video (MediaStore URI, or a path),
  /// clip start / cover time in ms, mute flag, and the generator revision that wrote the still and clip
  /// ([liveGenCurrent]). 0 = written before this was recorded (the false-colour 4:2:2 era).
  String? liveSrc;
  int? liveStartMs, liveCoverMs;
  bool liveMute;
  int liveGen;

  /// 源色彩 used (LiveSource index) and what the clip encode recorded (codec, profile, bit depth, audio, decoder).
  int liveSource;
  String? liveInfo;
  static const int liveGenCurrent = 2;

  /// Capture time when known, else the time it reached the phone.
  int get sortMs => takenMs ?? addedMs;

  bool get isRaw {
    final n = name.toLowerCase();
    return n.endsWith('.arw') || n.endsWith('.raf') || n.endsWith('.dng');
  }

  bool get openable => status != PhotoStatus.arriving && status != PhotoStatus.interrupted && status != PhotoStatus.failed;
  bool get processing => status == PhotoStatus.pending || status == PhotoStatus.developing;

  /// Chinese display title (file name without extension).
  String get title {
    final i = name.lastIndexOf('.');
    return i > 0 ? name.substring(0, i) : name;
  }

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'addedMs': addedMs,
        'uri': uri,
        'path': path,
        'camera': camera,
        'status': status.name,
        'previewPath': previewPath,
        'edit': edit.toJson(),
        'correction': correction,
        'error': error,
        'exported': exported,
        'flag': flag,
        'ratio': ratio,
        'pxW': pxW,
        'pxH': pxH,
        'exif': exif,
        'nearest': nearest,
        'takenMs': takenMs,
        'pairedJpeg': pairedJpeg,
        if (kind == PhotoKind.live) ...{'liveSrc': liveSrc, 'liveStartMs': liveStartMs, 'liveCoverMs': liveCoverMs, 'liveMute': liveMute, 'liveGen': liveGen, 'liveSource': liveSource, 'liveInfo': liveInfo},
        if (kind == PhotoKind.hdr || kind == PhotoKind.pano || kind == PhotoKind.live) ...{'kind': kind.name, 'badge': badge},
      };

  factory Photo.fromJson(Map<String, dynamic> j) => Photo(
        id: j['id'] as String,
        name: j['name'] as String,
        addedMs: (j['addedMs'] as num).toInt(),
        uri: j['uri'] as String?,
        path: j['path'] as String?,
        camera: j['camera'] as String?,
        status: PhotoStatus.values.firstWhere((s) => s.name == j['status'], orElse: () => PhotoStatus.pending),
        previewPath: j['previewPath'] as String?,
        edit: j['edit'] is Map ? EditState.fromJson((j['edit'] as Map).cast<String, dynamic>()) : EditState.empty,
        correction: (j['correction'] as Map?)?.cast<String, dynamic>(),
        error: j['error'] as String?,
        exported: (j['exported'] as bool?) ?? false,
        flag: (j['flag'] as bool?) ?? false,
        ratio: (j['ratio'] as num?)?.toDouble(),
        pxW: (j['pxW'] as num?)?.toInt(),
        pxH: (j['pxH'] as num?)?.toInt(),
        exif: j['exif'] as String?,
        nearest: j['nearest'] as String?,
        takenMs: (j['takenMs'] as num?)?.toInt(),
        pairedJpeg: j['pairedJpeg'] as String?,
        liveSrc: j['liveSrc'] as String?,
        liveStartMs: (j['liveStartMs'] as num?)?.toInt(),
        liveCoverMs: (j['liveCoverMs'] as num?)?.toInt(),
        liveMute: (j['liveMute'] as bool?) ?? false,
        liveGen: (j['liveGen'] as num?)?.toInt() ?? 0,
        liveSource: (j['liveSource'] as num?)?.toInt() ?? 0,
        liveInfo: j['liveInfo'] as String?,
        kind: PhotoKind.values.firstWhere((k) => k.name == j['kind'], orElse: () => PhotoKind.photo),
        badge: j['badge'] as String?,
      );
}

/// Global settings (requirements 4.13).
class AppSettings {
  AppSettings({
    this.defaultLookId = 'std',
    this.toneStrength = 60,
    this.autoProcess = true,
    this.exportLongEdge = 0,
    this.stripLocation = true,
    this.jpegQuality = 95,
    this.watchGallery = true,
    this.onboarded = false,
    this.ftpEnabled = false,
    this.ftpUser = 'dapple',
    this.ftpPass = '',
    this.ftpPort = 2121,
    this.handedness = 0,
    this.haptics = true,
    this.handSide = 1,
    this.editorLayout = '',
  });
  String defaultLookId;

  /// 0..100, default 60.
  double toneStrength;
  bool autoProcess;
  /// Long side in px for save / share; 0 = the photo's original size.
  int exportLongEdge;
  bool stripLocation;
  int jpegQuality;
  bool watchGallery;
  bool onboarded;

  /// Camera FTP receiver (Settings > 相机无线接收).
  bool ftpEnabled;
  String ftpUser;
  String ftpPass;
  int ftpPort;

  /// Editor handedness (requirements 4.15.12): 0 auto, 1 left, 2 right.
  int handedness;

  /// Landmark haptics in the editor (4.15.9).
  bool haptics;

  /// Last resolved dominant side (0 left, 1 right), kept across sessions.
  int handSide;

  /// Serialized floating window memory (WindowManager.toJsonString).
  String editorLayout;
}

/// Camera connection card (requirements 4.2.2).
enum ConnState { connected, interrupted, disconnected, idle }
