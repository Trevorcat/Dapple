import 'package:flutter/foundation.dart';

import '../models.dart';
import 'live_bridge.dart';
import 'merge_engine.dart';

enum ToolKind { hdr, pano, live }

extension ToolKindText on ToolKind {
  /// Result name used in toasts and badges.
  String get resultName => switch (this) { ToolKind.hdr => 'HDR', ToolKind.pano => '全景', ToolKind.live => '动态照片' };
  String get runningTitle => switch (this) {
        ToolKind.hdr => '正在合成 HDR',
        ToolKind.pano => '正在拼接全景',
        ToolKind.live => '正在生成动态照片',
      };
  String get engineKind => this == ToolKind.pano ? 'pano' : 'hdr';
  PhotoKind get photoKind => switch (this) { ToolKind.hdr => PhotoKind.hdr, ToolKind.pano => PhotoKind.pano, ToolKind.live => PhotoKind.live };
}

/// One candidate in the source strip. Unchecked candidates stay in the strip and can be checked again.
class DraftSource {
  DraftSource({required this.photoId, required this.label, required this.timeMs, this.checked = true, this.detected = false});
  final String photoId;

  /// Bottom label: EV (HDR), n/N (panorama) or HH:MM.
  final String label;
  final int timeMs;
  bool checked;

  /// Part of the auto-detected set ("识别" tag).
  final bool detected;

  DraftSource copy() => DraftSource(photoId: photoId, label: label, timeMs: timeMs, checked: checked, detected: detected);
}

/// State of the HDR / panorama tool page. Lives on after the page is closed so that a failed background job
/// can reopen the page with the same selection ("查看").
class MergeDraft {
  MergeDraft({required this.kind, required this.sources, this.baseId, this.note});
  final ToolKind kind;

  /// All candidates, oldest first (the order the engine receives them).
  final List<DraftSource> sources;
  String? baseId;

  /// "已识别 3 张包围曝光" / "已带入你选的 N 张" or null.
  String? note;

  String deghost = 'mid'; // off | low | mid | high
  bool align = true;
  String projection = 'cyl'; // sph | cyl | persp
  bool crop = true;
  bool fill = false;

  List<DraftSource> get checked => [for (final s in sources) if (s.checked) s];

  /// The base frame: the chosen one when still checked, else the middle checked frame.
  String? get effectiveBaseId {
    final c = checked;
    if (c.isEmpty) return null;
    if (baseId != null && c.any((s) => s.photoId == baseId)) return baseId;
    return c[c.length ~/ 2].photoId;
  }

  /// Index of the base frame inside the checked list (the engine's `base`).
  int get baseIndex {
    final id = effectiveBaseId;
    final c = checked;
    return id == null ? -1 : c.indexWhere((s) => s.photoId == id);
  }

  /// Failure recovery: the next checked frame becomes the base.
  void nextBase() {
    final c = checked;
    if (c.length < 2) return;
    final cur = c.indexWhere((s) => s.photoId == effectiveBaseId);
    baseId = c[(cur + 1) % c.length].photoId;
  }

  /// The engine options (dapple_c.h).
  Map<String, dynamic> options({String quality = 'full', int longEdge = 0}) {
    if (kind == ToolKind.hdr) {
      return {'base': baseIndex, 'deghost': deghost, 'align': align, 'quality': quality, 'longEdge': longEdge};
    }
    return {
      'projection': projection,
      'crop': crop,
      'fill': fill && !crop,
      'base': baseIndex,
      'exposureComp': true,
      'quality': quality,
      'longEdge': longEdge,
    };
  }

  /// Selection / parameters identical (used to decide whether the preview needs recomputing).
  String get signature =>
      '${kind.name}|${checked.map((s) => s.photoId).join(',')}|$baseIndex|$deghost|$align|$projection|$crop|$fill';
}

enum JobStatus { running, failed, done, cancelled }

/// A running (or finished) tool job: HDR merge, panorama stitch or Motion Photo creation.
class ToolJob extends ChangeNotifier {
  ToolJob({required this.kind, this.draft, this.live});
  final ToolKind kind;
  final MergeDraft? draft;
  final LiveDraft? live;

  JobStatus status = JobStatus.running;

  /// 0..1
  double fraction = 0;
  String? error;

  /// Moved to the background: the home screen shows the task bar, the tool page is gone.
  bool background = false;
  Photo? result;
  MergeHandle? handle;

  int get percent => (fraction * 100).round().clamp(0, 100);

  /// 对齐 → 合并 → 处理 by progress thirds (requirements 4.9.6).
  String get step => fraction < 0.34 ? '对齐' : (fraction < 0.68 ? '合并' : '处理');
  String get stepLine => kind == ToolKind.live ? '截取 → 封面 → 写入' : '对齐 → 合并 → 处理';

  void update({double? fraction, JobStatus? status, String? error, bool? background}) {
    if (fraction != null) this.fraction = fraction;
    if (status != null) this.status = status;
    if (error != null) this.error = error;
    if (background != null) this.background = background;
    notifyListeners();
  }
}

/// Parameters of the Motion Photo tool.
class LiveDraft {
  LiveDraft({required this.video, this.startSec = 0, this.coverSec = 1.5, this.mute = true});
  final VideoItem video;

  /// Window start within the video; the window is [windowSec] long.
  double startSec;

  /// Absolute time of the still within the video (kept inside the window).
  double coverSec;
  bool mute;

  static const double windowSec = 3.0;

  double get windowLen => video.seconds < windowSec ? video.seconds : windowSec;
  double get maxStart => (video.seconds - windowLen).clamp(0.0, double.infinity);

  void setStart(double s) {
    startSec = s.clamp(0.0, maxStart);
    coverSec = coverSec.clamp(startSec, startSec + windowLen);
  }

  void setCover(double c) => coverSec = c.clamp(startSec, startSec + windowLen);

  /// "0:06"
  static String clock(double s) {
    final t = s.floor();
    return '${t ~/ 60}:${(t % 60).toString().padLeft(2, '0')}';
  }
}
