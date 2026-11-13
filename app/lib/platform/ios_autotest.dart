// On-device measurement script (docs/ios/device-results.md). Only compiled in when the build was made with
//   --dart-define=DAPPLE_IOS_AUTOTEST=true
// and only does something when Documents/autotest.json exists (pushed with `pymobiledevice3 apps push`; the files to measure are
// pushed to Documents/Photos). It drives the engine through the FFI session API exactly like the editor does (no UI input), writes
// one JSON object per line to Documents/autotest_log.txt (pulled back with `apps pull`) and never touches the user's library.
//
// autotest.json: {"files":["a.ARW",..],"stocks":["kodak_portra_400"],"drags":12,"export":false}
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:path_provider/path_provider.dart';

import '../native/dapple_session.dart';
import '../services/ingest.dart';
import 'ios_frame_trace.dart';
import 'test_trigger.dart';

const bool kIosAutotest = bool.fromEnvironment('DAPPLE_IOS_AUTOTEST');

class IosAutotest {
  static bool _started = false;
  static late File _log;

  static void _w(Map<String, dynamic> m) {
    m['rssMB'] = (ProcessInfo.currentRss / 1048576).round();
    m['maxRssMB'] = (ProcessInfo.maxRss / 1048576).round();
    _log.writeAsStringSync('${jsonEncode(m)}\n', mode: FileMode.append, flush: true);
  }

  static double _median(List<double> v) {
    final s = [...v]..sort();
    return s.isEmpty ? 0 : s[s.length ~/ 2];
  }

  static Future<void> runIfRequested() async {
    if (!kIosAutotest || !Platform.isIOS || _started) return;
    final docs = (await getApplicationDocumentsDirectory()).path;
    final text = TestTrigger.consume(File('$docs/autotest.json')); // one-shot: deleted now, a later launch is a normal app
    if (text == null) return;
    _started = true;
    await TestTrigger.setActive(true);
    _log = File('$docs/autotest_log.txt');
    if (_log.existsSync()) _log.deleteSync();
    final cfg = jsonDecode(text) as Map<String, dynamic>;
    final drags = (cfg['drags'] as num?)?.toInt() ?? 12;
    final w = Stopwatch()..start();
    try {
      _w({'event': 'start', 'version': DappleSession.version, 'time': DateTime.now().toIso8601String()});
      // film pack
      final data = await rootBundle.load('assets/film/dapple.dfp');
      final okPack = DappleSession.loadFilmPack(data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes));
      _w({'event': 'film_pack', 'ok': okPack, 'list': (DappleSession.filmListJson() ?? '').length});
      // wait for the GPU probe started by AppDelegate (self-test + film check)
      String st = '';
      for (var i = 0; i < 240; i++) {
        st = DappleSession.gpuStatusJson() ?? '';
        if (st.contains('"state":"ready"') || st.contains('"state":"failed"')) break;
        await Future<void>.delayed(const Duration(milliseconds: 500));
      }
      _w({'event': 'gpu_status', 'waitedMs': w.elapsedMilliseconds, 'status': st.isEmpty ? null : jsonDecode(st)});
      final files = [for (final f in (cfg['files'] as List? ?? const [])) '$docs/Photos/$f'];
      final stocks = [for (final s in (cfg['stocks'] as List? ?? const [])) '$s'];
      for (final path in files) {
        if (!File(path).existsSync()) {
          _w({'event': 'missing', 'file': path});
          continue;
        }
        await _one(path, stocks, drags, cfg['export'] == true, docs, keepExport: cfg['keepExport'] == true);
      }
      final st2 = cfg['stress'];
      if (st2 is Map<String, dynamic>) await _stress(files.where((f) => File(f).existsSync()).toList(), (st2['seconds'] as num?)?.toInt() ?? 30, (st2['workers'] as num?)?.toInt() ?? 4);
      final tr = cfg['trace'];
      if (tr is Map<String, dynamic>) {
        final f = '$docs/Photos/${tr['file']}';
        if (File(f).existsSync()) await FrameTraceRun(docs, tr).run(f, _w);
      }
      _w({'event': 'gpu_status_end', 'status': jsonDecode(DappleSession.gpuStatusJson() ?? '{}')});
    } catch (e, s) {
      _w({'event': 'error', 'error': '$e', 'stack': '$s'.split('\n').take(6).toList()});
    }
    _w({'event': 'done', 'totalMs': w.elapsedMilliseconds});
    await TestTrigger.setActive(false);
  }

  /// Concurrency stress of the decode path (tests/engine/omp_stress_tests.cpp on the phone): [workers] loops open and close sessions (preview opens with the
  /// embedded-preview read on another thread, full opens of small files, regions, background prefetch opens) for [seconds]; one JSON line with the counts.
  static Future<void> _stress(List<String> files, int seconds, int workers) async {
    if (files.isEmpty) return;
    final end = DateTime.now().add(Duration(seconds: seconds));
    var opens = 0, fails = 0, crashed = 0;
    Future<void> loop(int id) async {
      var n = id * 7919;
      while (DateTime.now().isBefore(end)) {
        final f = files[(n + id) % files.length];
        final small = f.toLowerCase().endsWith('.dng');
        var kind = (n ~/ 3 + id) % 7;
        if (kind == 2 && !small) kind = 0;
        if (kind > 4) kind = 4;
        try {
          final s = kind == 3
              ? await DappleSession.openRegion(f, 100, 100, 1300, 900, background: id.isOdd)
              : await DappleSession.open(f, full: kind == 2, longEdge: kind == 1 ? 4096 : (kind == 4 ? 1024 : 2048), background: id.isOdd && kind == 0);
          await s.close();
        } catch (e) {
          if (kind != 3) fails++;
        }
        opens++;
        n++;
      }
    }
    try {
      await Future.wait([for (var i = 0; i < workers; i++) loop(i)]);
    } catch (e) {
      crashed++;
    }
    _w({'event': 'stress', 'workers': workers, 'seconds': seconds, 'files': files.length, 'opens': opens, 'failedOpens': fails, 'errors': crashed});
  }

  static Future<void> _one(String path, List<String> stocks, int drags, bool export, String docs, {bool keepExport = false}) async {
    final name = path.split('/').last;
    final lower = name.toLowerCase();
    if (lower.endsWith('.heic') || lower.endsWith('.heif') || lower.endsWith('.tif') || lower.endsWith('.tiff')) {
      // what the library does before the engine opens such a file: the platform decoder -> 16-bit PNG in its own colour space
      final t = Stopwatch()..start();
      final conv = await MediaBridge.toPng16(path, '$docs/autotest_conv_$name.png');
      _w({'event': 'convert', 'file': name, 'ms': t.elapsedMilliseconds, 'out': conv.split('/').last, 'bytes': File(conv).lengthSync()});
      path = conv;
    }
    final sw = Stopwatch()..start();
    final s = await DappleSession.open(path, longEdge: 2048);
    final openMs = sw.elapsedMilliseconds;
    final info = await s.info();
    _w({'event': 'open', 'file': name, 'openMs': openMs, 'info': info});
    Map<String, dynamic> base() => {'look': 'std', 'lookStrength': 100, 'toneStrength': 60};
    Future<double> frame(Map<String, dynamic> p, {bool interactive = false, int longEdge = 0}) async {
      final t = Stopwatch()..start();
      final f = await s.renderFrame(p, interactive: interactive, longEdge: longEdge, analysis: false);
      final ms = t.elapsedMicroseconds / 1000.0;
      f.dispose();
      return ms;
    }
    // plain pipeline: first frame, final frame, exposure drag
    final first = await frame(base(), longEdge: 1600);
    final fin = <double>[for (var i = 0; i < 5; i++) await frame(base(), longEdge: 1600)];
    final expo = <double>[
      for (var i = 0; i < drags; i++) await frame({...base(), 'exposure': -50.0 + 100.0 * i / (drags - 1)}, interactive: true, longEdge: 1080)
    ];
    _w({'event': 'plain', 'file': name, 'firstFrameMs': first, 'finalMedianMs': _median(fin), 'exposureDragMedianMs': _median(expo), 'exposureDragMs': expo});
    for (final stock in stocks) {
      final fp = {...base(), 'film': stock};
      final f1 = await frame(fp, longEdge: 1600);
      final ff = <double>[for (var i = 0; i < 5; i++) await frame(fp, longEdge: 1600)];
      final strength = <double>[
        for (var i = 0; i < drags; i++) await frame({...fp, 'film_strength': 100.0 - 80.0 * i / (drags - 1)}, interactive: true, longEdge: 1080)
      ];
      final fexp = <double>[
        for (var i = 0; i < drags; i++) await frame({...fp, 'exposure': -40.0 + 80.0 * i / (drags - 1)}, interactive: true, longEdge: 1080)
      ];
      _w({
        'event': 'film',
        'file': name,
        'stock': stock,
        'firstFrameMs': f1,
        'finalMedianMs': _median(ff),
        'strengthDragMedianMs': _median(strength),
        'exposureDragMedianMs': _median(fexp),
        'strengthDragMs': strength
      });
    }
    if (export) {
      final t = Stopwatch()..start();
      final out = '$docs/autotest_export_${name.split('.').first}.jpg';
      final sf = await DappleSession.open(path, full: true, longEdge: 0);
      final fullOpen = t.elapsedMilliseconds;
      final t2 = Stopwatch()..start();
      await sf.exportJpeg({...base(), if (stocks.isNotEmpty) 'film': stocks.first}, outPath: out);
      _w({'event': 'export', 'file': name, 'fullOpenMs': fullOpen, 'exportMs': t2.elapsedMilliseconds, 'bytes': File(out).existsSync() ? File(out).lengthSync() : -1});
      await sf.close();
      if (!keepExport) {
        try {
          File(out).deleteSync();
        } catch (_) {}
      }
    }
    await s.close();
    debugPrint('IosAutotest: $name done');
  }
}
