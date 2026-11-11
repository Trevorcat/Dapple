import 'dart:async';
import 'dart:io';

import 'package:flutter/gestures.dart' show DragStartBehavior;
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;

import '../../services/live_bridge.dart';
import '../../services/tool_models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import 'progress_dialog.dart';
import 'tool_common.dart';

/// 视频转动态照片 (Android wording of requirements 4.9.5): pick a video, place the fixed 3.0 s window and the
/// cover on a filmstrip, write a Motion Photo and save it to Pictures/Dapple.
class LiveToolPage extends StatefulWidget {
  const LiveToolPage({super.key, required this.app, this.video});
  final AppState app;
  final VideoItem? video;

  @override
  State<LiveToolPage> createState() => _LiveToolPageState();
}

enum _Drag { none, window, cover }

class _LiveToolPageState extends State<LiveToolPage> {
  AppState get app => widget.app;
  LiveDraft? d;
  double _aspect = 3 / 4;
  List<String?> _thumbs = const [];
  String? _coverPath;
  int _coverRev = 0;
  Timer? _coverTimer;
  bool _coverBusy = false;
  _Drag _drag = _Drag.none;

  // press and hold on the preview plays the window as a quick frame sequence
  List<String?> _playFrames = const [];
  Timer? _playTimer;
  int _playIdx = 0;
  bool _playing = false;
  String _playKey = '';

  static const int _cells = 10;

  @override
  void initState() {
    super.initState();
    if (widget.video != null) {
      _setVideo(widget.video!);
    } else {
      unawaited(app.refreshVideos());
    }
  }

  @override
  void dispose() {
    _coverTimer?.cancel();
    _playTimer?.cancel();
    super.dispose();
  }

  Future<void> _setVideo(VideoItem v) async {
    final nd = LiveDraft(video: v);
    nd.setStart(((v.seconds - nd.windowLen) / 2).clamp(0.0, nd.maxStart));
    nd.coverSec = nd.startSec + nd.windowLen / 2;
    setState(() {
      d = nd;
      _thumbs = const [];
      _coverPath = null;
      _playFrames = const [];
    });
    unawaited(_loadInfo(v));
    unawaited(_loadThumbs(v));
    _scheduleCover(immediate: true);
  }

  Future<void> _loadInfo(VideoItem v) async {
    try {
      final i = await LiveBridge.info(v.uri);
      final w = (i['width'] as num?)?.toDouble() ?? 0, h = (i['height'] as num?)?.toDouble() ?? 0;
      final rot = (i['rotation'] as num?)?.toInt() ?? 0;
      if (w > 0 && h > 0 && mounted && d?.video == v) {
        final a = (rot == 90 || rot == 270) ? h / w : w / h;
        setState(() => _aspect = a.clamp(0.4, 2.4).toDouble());
      }
    } catch (_) {}
  }

  Future<void> _loadThumbs(VideoItem v) async {
    try {
      final times = [for (var i = 0; i < _cells; i++) ((i + 0.5) / _cells * v.durationMs).round()];
      final l = await LiveBridge.thumbs(v.uri, times, maxEdge: 200, destDir: app.scratchPath, prefix: 'film_${v.id}');
      if (mounted && d?.video == v) setState(() => _thumbs = l);
    } catch (_) {}
  }

  void _scheduleCover({bool immediate = false}) {
    _coverTimer?.cancel();
    _coverTimer = Timer(immediate ? Duration.zero : const Duration(milliseconds: 280), _loadCover);
  }

  Future<void> _loadCover() async {
    final cur = d;
    if (cur == null) return;
    final rev = ++_coverRev;
    setState(() => _coverBusy = true);
    try {
      final out = p.join(app.scratchPath, 'live_cover_preview_$rev.jpg');
      await LiveBridge.frame(cur.video.uri, (cur.coverSec * 1000).round(), out, maxEdge: 900, quality: 85);
      if (!mounted || rev != _coverRev) return;
      imageCache.clear();
      setState(() => _coverPath = out);
    } catch (_) {
      // keep the previous frame
    } finally {
      if (mounted && rev == _coverRev) setState(() => _coverBusy = false);
    }
  }

  // ---- filmstrip gestures
  double _frac(double dx, double w) => (dx / w).clamp(0.0, 1.0);

  void _dragStart(double dx, double w) {
    final cur = d;
    if (cur == null) return;
    final dur = cur.video.seconds;
    final f = _frac(dx, w);
    final coverF = cur.coverSec / dur;
    final winA = cur.startSec / dur, winB = (cur.startSec + cur.windowLen) / dur;
    if ((f - coverF).abs() <= 0.04) {
      _drag = _Drag.cover;
    } else if (f >= winA && f <= winB) {
      _drag = _Drag.window;
    } else {
      // tap outside: centre the window there
      cur.setStart(f * dur - cur.windowLen / 2);
      cur.setCover(cur.startSec + cur.windowLen / 2);
      _drag = _Drag.window;
      setState(() {});
      _scheduleCover();
    }
  }

  void _dragUpdate(double ddx, double w) {
    final cur = d;
    if (cur == null || _drag == _Drag.none) return;
    final dd = ddx / w * cur.video.seconds;
    setState(() {
      if (_drag == _Drag.window) {
        final off = cur.coverSec - cur.startSec;
        cur.setStart(cur.startSec + dd);
        cur.setCover(cur.startSec + off);
      } else {
        cur.setCover(cur.coverSec + dd);
      }
    });
    _scheduleCover();
  }

  // ---- hold to play
  Future<void> _holdStart() async {
    final cur = d;
    if (cur == null) return;
    final key = '${cur.video.id}:${cur.startSec.toStringAsFixed(2)}';
    if (_playKey != key || _playFrames.isEmpty) {
      try {
        final times = [for (var i = 0; i < 12; i++) ((cur.startSec + cur.windowLen * i / 11) * 1000).round()];
        _playFrames = await LiveBridge.thumbs(cur.video.uri, times,
            maxEdge: 480, destDir: app.scratchPath, prefix: 'play_${cur.video.id}');
        _playKey = key;
      } catch (_) {
        return;
      }
    }
    if (!mounted) return;
    _playIdx = 0;
    setState(() => _playing = true);
    _playTimer?.cancel();
    _playTimer = Timer.periodic(const Duration(milliseconds: 250), (_) {
      if (_playFrames.isEmpty) return;
      setState(() => _playIdx = (_playIdx + 1) % _playFrames.length);
    });
  }

  void _holdEnd() {
    _playTimer?.cancel();
    if (_playing) setState(() => _playing = false);
  }

  Future<void> _pickVideo() async {
    await app.refreshVideos();
    if (!mounted) return;
    final v = await showModalBottomSheet<VideoItem>(
      context: context,
      isScrollControlled: true,
      useSafeArea: true,
      backgroundColor: context.c.bg,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
      builder: (_) => _VideoList(app: app),
    );
    if (v != null) await _setVideo(v);
  }

  Future<void> _start() async {
    final cur = d;
    if (cur == null) return;
    final job = await app.startLive(cur);
    if (!mounted) return;
    final r = await showToolProgress(context, app, job);
    if (!mounted) return;
    if (r == ToolDialogResult.done) {
      Navigator.of(context).pop();
      app.showToast('动态照片已加入「今天」');
    } else if (r == ToolDialogResult.background) {
      Navigator.of(context).pop();
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final cur = d;
    final size = MediaQuery.sizeOf(context);
    return ToolScaffold(
      title: '视频转动态照片',
      onCancel: () => Navigator.of(context).pop(),
      mainLabel: cur == null ? '选择视频' : '生成动态照片',
      onMain: cur == null ? _pickVideo : _start,
      body: cur == null ? _empty(context) : _editor(context, cur, size, c),
    );
  }

  Widget _empty(BuildContext context) {
    final c = context.c;
    return Center(
      child: Column(mainAxisSize: MainAxisSize.min, children: [
        Text(app.videos.isEmpty ? '相册里没有视频' : '从相册选一段视频', style: T.body17(c.text2)),
      ]),
    );
  }

  Widget _editor(BuildContext context, LiveDraft cur, Size size, DC c) {
    final maxH = size.height * 0.30;
    var h = maxH, w = h * _aspect;
    if (w > size.width - 32) {
      w = size.width - 32;
      h = w / _aspect;
    }
    final coverText = '封面 ${LiveDraft.clock(cur.coverSec)}';
    final shown = _playing && _playFrames.isNotEmpty ? _playFrames[_playIdx % _playFrames.length] : _coverPath;
    return ListView(padding: const EdgeInsets.only(bottom: Sp.l), children: [
      Center(
        child: GestureDetector(
          key: const ValueKey('live-preview'),
          onLongPressStart: (_) => _holdStart(),
          onLongPressEnd: (_) => _holdEnd(),
          onLongPressCancel: _holdEnd,
          child: SizedBox(
            width: w,
            height: h,
            child: PreviewBox(
              busy: _coverBusy && _coverPath == null,
              badge: '动态照片 · ${cur.windowLen.toStringAsFixed(1)} 秒',
              tag: _playing ? '播放中' : coverText,
              child: shown == null ? const SizedBox.shrink() : Image.file(File(shown), fit: BoxFit.cover, gaplessPlayback: true, errorBuilder: (_, _, _) => const SizedBox.shrink()),
            ),
          ),
        ),
      ),
      const SizedBox(height: Sp.l),
      Padding(
        padding: const EdgeInsets.symmetric(horizontal: Sp.l),
        child: Row(children: [
          Expanded(
              child: Text('${cur.windowLen.toStringAsFixed(0)} 秒片段 · 视频 ${LiveDraft.clock(cur.video.seconds)}',
                  key: const ValueKey('live-strip-title'), style: T.body(c.text2, w: FontWeight.w500, tabular: true))),
          KeyedSubtree(key: const ValueKey('live-change'), child: TextAction('更换视频', size: 15, bold: true, onPressed: _pickVideo)),
        ]),
      ),
      const SizedBox(height: Sp.s),
      Padding(
        padding: const EdgeInsets.symmetric(horizontal: Sp.l),
        child: Semantics(
          label: '${cur.windowLen.toStringAsFixed(0)} 秒片段起点 ${LiveDraft.clock(cur.startSec)}，封面 ${LiveDraft.clock(cur.coverSec)}，左右滑动移动片段',
          slider: true,
          child: LayoutBuilder(builder: (context, box) {
            final sw = box.maxWidth;
            return GestureDetector(
              key: const ValueKey('live-film'),
              behavior: HitTestBehavior.opaque,
              dragStartBehavior: DragStartBehavior.down,
              onPanStart: (e) => _dragStart(e.localPosition.dx, sw),
              onPanUpdate: (e) => _dragUpdate(e.delta.dx, sw),
              onPanEnd: (_) => _drag = _Drag.none,
              onTapDown: (e) {
                _dragStart(e.localPosition.dx, sw);
                _drag = _Drag.none;
              },
              child: SizedBox(
                height: 56,
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(Rd.thumb),
                  child: CustomPaint(
                    foregroundPainter: _FilmPainter(
                      start: cur.startSec / cur.video.seconds,
                      end: (cur.startSec + cur.windowLen) / cur.video.seconds,
                      cover: cur.coverSec / cur.video.seconds,
                    ),
                    child: Row(children: [
                      for (var i = 0; i < _cells; i++)
                        Expanded(
                          child: i < _thumbs.length && _thumbs[i] != null
                              ? Image.file(File(_thumbs[i]!), fit: BoxFit.cover, height: 56, gaplessPlayback: true, errorBuilder: (_, _, _) => const SizedBox.shrink())
                              : ColoredBox(color: c.placeholder),
                        ),
                    ]),
                  ),
                ),
              ),
            );
          }),
        ),
      ),
      const SizedBox(height: Sp.l),
      ToolBlock(children: [
        ToolSwitchRow(
            switchKey: const ValueKey('live-mute'),
            label: '静音',
            value: cur.mute,
            onChanged: (v) => setState(() => cur.mute = v)),
      ]),
    ]);
  }
}

class _FilmPainter extends CustomPainter {
  _FilmPainter({required this.start, required this.end, required this.cover});
  final double start, end, cover;

  @override
  void paint(Canvas canvas, Size size) {
    final a = start * size.width, b = end * size.width;
    final dim = Paint()..color = const Color(0x80000000);
    canvas.drawRect(Rect.fromLTRB(0, 0, a, size.height), dim);
    canvas.drawRect(Rect.fromLTRB(b, 0, size.width, size.height), dim);
    canvas.drawRRect(
        RRect.fromRectAndRadius(Rect.fromLTRB(a + 1.5, 1.5, b - 1.5, size.height - 1.5), const Radius.circular(6)),
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 3
          ..color = Fx.star);
    final cx = cover * size.width;
    canvas.drawRect(Rect.fromLTWH(cx - 2, 0, 4, size.height), Paint()..color = Colors.white);
  }

  @override
  bool shouldRepaint(_FilmPainter o) => o.start != start || o.end != end || o.cover != cover;
}

class _VideoList extends StatelessWidget {
  const _VideoList({required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final list = app.videos;
    return Column(children: [
      SizedBox(
          height: 64,
          child: Row(children: [
            const SizedBox(width: 4),
            SizedBox(
                width: 80,
                child: Align(
                    alignment: Alignment.centerLeft,
                    child: Theme.of(context).platform == TargetPlatform.android
                        ? IconButton(tooltip: '关闭', icon: const Icon(Icons.close), color: c.text, onPressed: () => Navigator.of(context).pop())
                        : TextAction('取消', size: 17, onPressed: () => Navigator.of(context).pop()))),
            Expanded(child: Text('选择视频', textAlign: TextAlign.center, style: T.body17(c.text, w: FontWeight.w600))),
            const SizedBox(width: 84),
          ])),
      Expanded(
        child: list.isEmpty
            ? Center(child: Text('相册里没有视频', style: T.body17(c.text2)))
            : ListView.builder(
                key: const ValueKey('video-list'),
                itemCount: list.length,
                itemBuilder: (_, i) {
                  final v = list[i];
                  return InkWell(
                    key: ValueKey('video-${v.id}'),
                    onTap: () => Navigator.of(context).pop(v),
                    child: Container(
                      constraints: const BoxConstraints(minHeight: 64),
                      padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
                      child: Row(children: [
                        Expanded(
                          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                            Text(v.name, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body17(c.text)),
                            Text('${LiveDraft.clock(v.seconds)} · ${(v.size / 1048576).toStringAsFixed(0)} MB',
                                style: T.small(c.text2, tabular: true)),
                          ]),
                        ),
                      ]),
                    ),
                  );
                },
              ),
      ),
    ]);
  }
}
