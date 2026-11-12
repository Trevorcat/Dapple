import 'dart:async';
import 'dart:convert';
import 'dart:ui' as ui;

import 'package:flutter/scheduler.dart';
import 'package:flutter/foundation.dart' show BindingBase;
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

/// Measurement hook for the low-end work. Enabled only with `--dart-define=DAPPLE_PERF=true` (release builds
/// included); otherwise every call is a constant-folded no-op. Output goes to logcat as `DPERF {json}` lines,
/// post-processed by tools/throttle/perf_report.py.
const bool kPerfProbe = bool.fromEnvironment('DAPPLE_PERF');

/// Adds per-phase frame timing (needs [kPerfProbe] too): `--dart-define=DAPPLE_PERF_PHASES=true`.
const bool kPerfPhases = bool.fromEnvironment('DAPPLE_PERF_PHASES');

class PerfProbe {
  PerfProbe._();
  static bool _started = false;
  static bool dragging = false;

  static void start() {
    if (!kPerfProbe || _started) return;
    _started = true;
    SchedulerBinding.instance.addTimingsCallback((List<ui.FrameTiming> ts) {
      for (final t in ts) {
        log('timing', {
          'ui': t.buildDuration.inMicroseconds / 1000.0,
          'raster': t.rasterDuration.inMicroseconds / 1000.0,
          'total': t.totalSpan.inMicroseconds / 1000.0,
          'drag': dragging,
        });
      }
    });
  }

  static final Map<String, int> _counts = {};
  static final Map<String, int> frameCounts = {};
  static bool _countTimer = false;

  /// Rebuild counter (perf builds only): one `rebuilds` line per second with the counts of that second.
  static void count(String name) {
    if (!kPerfProbe) return;
    _counts[name] = (_counts[name] ?? 0) + 1;
    frameCounts[name] = (frameCounts[name] ?? 0) + 1;
    if (_countTimer) return;
    _countTimer = true;
    Timer.periodic(const Duration(seconds: 1), (_) {
      if (_counts.isEmpty) return;
      log('rebuilds', {'drag': dragging, ..._counts});
      _counts.clear();
    });
  }

  /// Time a section (perf builds only): adds its microseconds to the `<name>_us` counter of the second.
  static int begin() => kPerfProbe ? DateTime.now().microsecondsSinceEpoch : 0;
  static void end(String name, int t0) {
    if (!kPerfProbe) return;
    count('${name}_n');
    final d = DateTime.now().microsecondsSinceEpoch - t0;
    _counts['${name}_us'] = (_counts['${name}_us'] ?? 0) + d;
    frameCounts['${name}_us'] = (frameCounts['${name}_us'] ?? 0) + d;
  }

  /// Cost of one text layout on this device (perf builds only): ASCII number, CJK label + number.
  static void textBench() {
    if (!kPerfProbe) return;
    const style = TextStyle(fontSize: 13, height: 18 / 13, fontFeatures: [FontFeature.tabularFigures()]);
    double run(String Function(int) f) {
      final sw = Stopwatch()..start();
      for (var i = 0; i < 100; i++) {
        (TextPainter(text: TextSpan(text: f(i), style: style), textDirection: TextDirection.ltr, maxLines: 1)..layout()).dispose();
      }
      return sw.elapsedMicroseconds / 100.0;
    }
    log('textbench', {'ascii_us': run((i) => '+0.${i % 90 + 10}'), 'cjk_us': run((i) => '曝光  +0.${i % 90 + 10}')});
  }

  static void log(String ev, Map<String, Object?> data) {
    if (!kPerfProbe) return;
    // ignore: avoid_print
    print('DPERF ${jsonEncode({'ev': ev, 't': DateTime.now().microsecondsSinceEpoch ~/ 1000, ...data})}');
  }
}

/// Perf builds only: a binding that splits the UI-thread frame into its transient callbacks (tickers, springs),
/// the widget build and the layout / paint / composite that follows it.
class PerfBinding {
  static WidgetsBinding ensureInitialized() {
    if (!kPerfProbe || !kPerfPhases) return WidgetsFlutterBinding.ensureInitialized();
    return _PerfBindingImpl.instance;
  }
}

class _PerfBindingImpl extends BindingBase
    with GestureBinding, SchedulerBinding, ServicesBinding, PaintingBinding, SemanticsBinding, RendererBinding, WidgetsBinding {
  static WidgetsBinding get instance => BindingBase.checkInstance(_inst ??= _PerfBindingImpl());
  static _PerfBindingImpl? _inst;

  @override
  void handleBeginFrame(Duration? rawTimeStamp) {
    PerfProbe.frameCounts.clear();
    _fT0 = PerfProbe.begin();
    super.handleBeginFrame(rawTimeStamp);
    PerfProbe.end('f_trans', _fT0);
  }

  int _fT0 = 0;

  @override
  void drawFrame() {
    // WidgetsBinding.drawFrame, phase by phase (perf builds only)
    final t0 = PerfProbe.begin();
    if (rootElement != null) buildOwner!.buildScope(rootElement!);
    PerfProbe.end('p_build', t0);
    var t = PerfProbe.begin();
    rootPipelineOwner.flushLayout();
    PerfProbe.end('p_layout', t);
    t = PerfProbe.begin();
    rootPipelineOwner.flushCompositingBits();
    rootPipelineOwner.flushPaint();
    PerfProbe.end('p_paint', t);
    t = PerfProbe.begin();
    if (sendFramesToEngine) {
      for (final RenderView v in renderViews) {
        v.compositeFrame();
      }
      PerfProbe.end('p_comp', t);
      rootPipelineOwner.flushSemantics();
    }
    buildOwner!.finalizeTree();
    final total = PerfProbe.begin() - _fT0;
    if (total > 12000) PerfProbe.log('slow', {'total_us': total, 'drag': PerfProbe.dragging, ...PerfProbe.frameCounts});
  }
}
