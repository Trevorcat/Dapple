// Frame-trace scenarios of the on-device autotest (docs/engine/pipeline-architecture.md "Interactive film: stock sliding and pad drags").
// Drives the engine like the editor does for a drag: a native RenderLoop (null presenter: frames are rendered and read back, nothing is drawn) fed with
// latest-wins snapshots by timers, the AdaptiveQuality controller fed with the frame events, interaction + touch hints around the drag, a settled frame after
// the release. The engine's frame trace (dapple_frame_trace_*) records what each frame showed; the Dart side writes what was submitted and what the loop reported.
// Config (autotest.json): "trace": {"file":"a7m5.ARW","base":"<stock id>","slide":[8 stock ids],"padStocks":[2 stock ids],"slideMs":150,"dragMs":2000,"tag":"before"}
import 'dart:async';
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';

import '../native/dapple_bindings.dart';
import '../native/dapple_session.dart';
import '../services/adaptive_quality.dart';

typedef _FrameFnC = ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Int64, ffi.Int32, ffi.Int32, ffi.Int32, ffi.Double, ffi.Double, ffi.Double);

class FrameTraceRun {
  FrameTraceRun(this.docs, this.cfg);
  final String docs;
  final Map<String, dynamic> cfg;
  late final ffi.DynamicLibrary _lib = DappleBindings.openLibrary();
  late final _create = _lib.lookupFunction<ffi.Pointer<ffi.Void> Function(ffi.Pointer<ffi.Int32>), ffi.Pointer<ffi.Void> Function(ffi.Pointer<ffi.Int32>)>('dapple_view_create');
  late final _destroy = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>), void Function(ffi.Pointer<ffi.Void>)>('dapple_view_destroy');
  late final _attach = _lib.lookupFunction<ffi.Int32 Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Void>, ffi.Int32, ffi.Int32),
      int Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Void>, int, int)>('dapple_view_attach_window');
  late final _bind = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Void>), void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Void>)>('dapple_view_bind_session');
  late final _submit = _lib.lookupFunction<ffi.Int64 Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<DappleFrameRequestStruct>),
      int Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<DappleFrameRequestStruct>)>('dapple_view_submit');
  late final _setCb = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.NativeFunction<_FrameFnC>>, ffi.Pointer<ffi.Void>),
      void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.NativeFunction<_FrameFnC>>, ffi.Pointer<ffi.Void>)>('dapple_view_set_callback');
  late final _trEnable = _lib.lookupFunction<ffi.Void Function(ffi.Int32), void Function(int)>('dapple_frame_trace_enable');
  late final _trClear = _lib.lookupFunction<ffi.Void Function(), void Function()>('dapple_frame_trace_clear');
  late final _trDump = _lib.lookupFunction<ffi.Int32 Function(ffi.Pointer<Utf8>), int Function(ffi.Pointer<Utf8>)>('dapple_frame_trace_dump');

  ffi.Pointer<ffi.Void> _view = ffi.nullptr;
  ffi.Pointer<ffi.Void> _win = ffi.nullptr;
  ffi.NativeCallable<_FrameFnC>? _cb;
  final AdaptiveQuality quality = AdaptiveQuality(budgetMs: AdaptiveQuality.budgetForRefresh(60));
  bool _skipRecord = false;
  final Stopwatch _sw = Stopwatch();
  final List<String> _events = [];
  int _lastSeq = 0, _outOfOrder = 0, _inFlightMax = 0, _submitted = 0, _done = 0;
  late DappleSession _s;
  int finalLongEdge = 1600;
  double _lastRenderMs = 0;
  final Map<int, bool> _isInteractive = {};

  void _onFrame(ffi.Pointer<ffi.Void> u, int seq, int status, int w, int h, double r, double p, double lat) {
    if (seq < _lastSeq) _outOfOrder++;
    _lastSeq = seq;
    _done = seq;
    _lastRenderMs = r;
    _events.add('F\t${_sw.elapsedMicroseconds / 1000.0}\t$seq\t$status\t$w\t$h\t$r\t$p\t$lat\t${quality.rung}');
    if (_isInteractive[seq] == true) {
      final ms = r > p ? r : p;
      if (_skipRecord) {
        _skipRecord = false;
      } else if (quality.record(ms)) {
        _skipRecord = true;
        _events.add('R\t${_sw.elapsedMicroseconds / 1000.0}\t${quality.rung}');
      }
    }
  }

  int _submitOne(Map<String, dynamic> params, {required bool interactive, String? active}) {
    final pj = jsonEncode(params).toNativeUtf8();
    final rq = calloc<DappleFrameRequestStruct>();
    final ap = (active == null) ? ffi.nullptr : active.toNativeUtf8();
    try {
      final rung = quality.rung;
      rq.ref
        ..size = ffi.sizeOf<DappleFrameRequestStruct>()
        ..longEdge = interactive ? rung.longEdge : finalLongEdge
        ..flags = (interactive ? dappleFrameInteractive : 0) | dappleFrameAnalysis
        ..bakeSize = interactive ? rung.bake : 0
        ..activeParam = ap;
      final seq = _submit(_view, pj, ffi.nullptr, rq);
      _isInteractive[seq] = interactive;
      _submitted = seq;
      if (_submitted - _done > _inFlightMax) _inFlightMax = _submitted - _done;
      _events.add('S\t${_sw.elapsedMicroseconds / 1000.0}\t$seq\t${interactive ? 1 : 0}\t${interactive ? rung.longEdge : finalLongEdge}\t${jsonEncode(params)}');
      return seq;
    } finally {
      malloc.free(pj);
      if (ap != ffi.nullptr) malloc.free(ap);
      calloc.free(rq);
    }
  }

  Future<void> _settle() async {
    final target = _submitted;
    for (var i = 0; i < 4000 && _done < target; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
  }

  void _begin() {
    quality.beginDrag();
    DappleBindings.instanceOrNull?.setInteraction?.call(1);
    DappleSession.touchHint(_s.address, 80);
  }

  Future<void> _end(Map<String, dynamic> params) async {
    quality.endDrag();
    DappleBindings.instanceOrNull?.setInteraction?.call(0);
    await Future<void>.delayed(const Duration(milliseconds: 50));
    _submitOne(params, interactive: false);
    await _settle();
    await Future<void>.delayed(const Duration(milliseconds: 300));
  }

  void _phaseStart() {
    _events.clear();
    _trClear();
    _sw
      ..reset()
      ..start();
    _outOfOrder = 0;
    _inFlightMax = 0;
  }

  void _phaseDump(String name) {
    final tag = '${cfg['tag'] ?? 'run'}';
    final p = '$docs/trace_${tag}_$name';
    final c = '$p.tsv'.toNativeUtf8();
    _trDump(c);
    malloc.free(c);
    File('$p.events.txt').writeAsStringSync('${_events.join('\n')}\n');
    File('$p.meta.txt').writeAsStringSync(jsonEncode({'name': name, 'outOfOrderCallbacks': _outOfOrder, 'maxSubmitMinusDone': _inFlightMax, 'rungEnd': '${quality.rung}'}));
  }

  Future<void> run(String path, void Function(Map<String, dynamic>) log) async {
    _s = await DappleSession.open(path, longEdge: 2048);
    await _s.info();
    final opt = calloc<ffi.Int32>(2);
    opt[0] = 8;
    opt[1] = 3; // TIMER_LOOP | NULL_PRESENTER: the native loop on its 60 Hz grid, frames rendered (and read back), nothing drawn
    _view = _create(opt);
    calloc.free(opt);
    if (_view == ffi.nullptr) {
      log({'event': 'trace_error', 'error': 'view create failed'});
      return;
    }
    _win = calloc<ffi.Int32>(1).cast();
    _attach(_view, _win, 1080, 1440);
    _cb = ffi.NativeCallable<_FrameFnC>.listener(_onFrame);
    _setCb(_view, _cb!.nativeFunction, ffi.nullptr);
    _bind(_view, ffi.Pointer.fromAddress(_s.address));
    if (cfg['prebake'] == true) {
      DappleSession.startFilmPrebake();
      final tp = Stopwatch()..start();
      String st = '';
      for (var i = 0; i < 600; i++) {
        st = DappleSession.filmPrebakeStatus() ?? '';
        if (st.contains('"finished":true')) break;
        await Future<void>.delayed(const Duration(milliseconds: 500));
      }
      log({'event': 'prebake', 'waitMs': tp.elapsedMilliseconds, 'status': st.isEmpty ? null : jsonDecode(st)});
    }
    _trEnable(1);
    final base = '${cfg['base']}'; // stock ids come from autotest.json (no stock names in the UI sources)
    final slide = [for (final x in (cfg['slide'] as List)) '$x'];
    final padStocks = [for (final x in (cfg['padStocks'] as List? ?? const [])) '$x'];
    final slideMs = (cfg['slideMs'] as num?)?.toInt() ?? 150;
    final dragMs = (cfg['dragMs'] as num?)?.toInt() ?? 2000;
    Map<String, dynamic> pr(String stock, [Map<String, dynamic> extra = const {}]) => {'look': 'std', 'lookStrength': 100, 'toneStrength': 60, 'film': stock, ...extra};
    // the photo is open with a stock chosen: one settled frame
    _phaseStart();
    _submitOne(pr(base), interactive: false);
    await _settle();
    await Future<void>.delayed(const Duration(milliseconds: 500));
    _phaseDump('warm');
    // the app seeds the first drag rung from the engine's warm-up measurement: median interactive frame at 1080 px of this photo, per megapixel
    {
      final ms = <double>[];
      _begin();
      for (var i = 0; i < 6; i++) {
        final before = _done;
        _submitOne(pr(base, {'exposure': -5.0 + i}), interactive: true);
        for (var k = 0; k < 400 && _done <= before; k++) {
          await Future<void>.delayed(const Duration(milliseconds: 5));
        }
        ms.add(_lastRenderMs);
      }
      await _end(pr(base));
      ms.sort();
      final perMpx = ms[ms.length ~/ 2] / (1080 * 720 / 1e6);
      quality.seedFromMeasurement(perMpx);
      log({'event': 'seed', 'msPerMpx': perMpx, 'rung': '${quality.rung}'});
    }
    // scenario A: slide across the stocks, twice (first visits, then the same stocks again)
    for (final pass in [1, 2]) {
      _phaseStart();
      _begin();
      await Future<void>.delayed(const Duration(milliseconds: 100)); // finger down, the touch hint's burst runs before the first move
      final t = Stopwatch()..start();
      for (var i = 0; i < slide.length; i++) {
        final due = i * slideMs;
        while (t.elapsedMilliseconds < due) {
          await Future<void>.delayed(const Duration(milliseconds: 2));
        }
        _submitOne(pr(slide[i]), interactive: true);
      }
      while (t.elapsedMilliseconds < slide.length * slideMs) {
        await Future<void>.delayed(const Duration(milliseconds: 2));
      }
      await _end(pr(slide.last));
      _phaseDump('A$pass');
      log({'event': 'trace_phase', 'name': 'A$pass', 'ms': _sw.elapsedMilliseconds});
    }
    // scenario B: two pads, a diagonal across each for dragMs at 60 Hz input, two stocks
    for (final stock in padStocks) {
      _phaseStart();
      _submitOne(pr(stock), interactive: false); // the stock is chosen and settled before the pad is touched
      await _settle();
      await Future<void>.delayed(const Duration(milliseconds: 500));
      for (final pad in ['exposure', 'texture']) {
        _phaseStart();
        _begin();
        await Future<void>.delayed(const Duration(milliseconds: 100));
        final t = Stopwatch()..start();
        Map<String, dynamic> at(double u) => pad == 'exposure'
            ? {'film_shoot': -0.7 + 1.4 * u, 'film_print': -0.7 + 1.4 * u}
            : {'film_wb': u, 'film_black': -0.7 + 1.4 * u};
        final active = pad == 'exposure' ? 'film_shoot' : 'film_wb';
        var n = 0;
        while (t.elapsedMilliseconds < dragMs) {
          final due = (n * 1000 / 60).round();
          while (t.elapsedMilliseconds < due) {
            await Future<void>.delayed(const Duration(milliseconds: 1));
          }
          n++;
          _submitOne(pr(stock, at((t.elapsedMilliseconds / dragMs).clamp(0.0, 1.0))), interactive: true, active: active);
        }
        await _end(pr(stock, at(1.0)));
        _phaseDump('B_${pad}_$stock');
        log({'event': 'trace_phase', 'name': 'B_${pad}_$stock', 'ms': _sw.elapsedMilliseconds});
      }
    }
    _trEnable(0);
    _bind(_view, ffi.nullptr);
    _destroy(_view);
    _cb?.close();
    await _s.close();
  }
}
