import 'dart:async';
import 'dart:convert';
import 'dart:ui' as ui;

import 'package:flutter/scheduler.dart';

/// Measurement hook for the low-end work. Enabled only with `--dart-define=DAPPLE_PERF=true` (release builds
/// included); otherwise every call is a constant-folded no-op. Output goes to logcat as `DPERF {json}` lines,
/// post-processed by tools/throttle/perf_report.py.
const bool kPerfProbe = bool.fromEnvironment('DAPPLE_PERF');

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
  static bool _countTimer = false;

  /// Rebuild counter (perf builds only): one `rebuilds` line per second with the counts of that second.
  static void count(String name) {
    if (!kPerfProbe) return;
    _counts[name] = (_counts[name] ?? 0) + 1;
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
    _counts['${name}_us'] = (_counts['${name}_us'] ?? 0) + DateTime.now().microsecondsSinceEpoch - t0;
  }

  static void log(String ev, Map<String, Object?> data) {
    if (!kPerfProbe) return;
    // ignore: avoid_print
    print('DPERF ${jsonEncode({'ev': ev, 't': DateTime.now().microsecondsSinceEpoch ~/ 1000, ...data})}');
  }
}
