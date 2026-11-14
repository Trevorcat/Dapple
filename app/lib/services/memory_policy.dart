// What the app gives back, and when (docs/engine/pipeline-architecture.md 42.9):
//  - start-up: the device's memory class goes to the engine (export ceiling, mosaic cache budget); the Flutter image cache is bounded
//  - the editor closed: its sessions (photo, neighbours, 2x / 1:1 detail) are closed and the engine trims its caches (dapple_memory_trim 1)
//  - a full-size export: the sessions the export does not use are closed first (the engine trims the rest at the FULL open)
//  - Android onTrimMemory (MemoryPlugin.kt): engine trim 1 / 2 and the Flutter image cache
// Nothing here runs periodically.
import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show debugPrint, kIsWeb;
import 'package:flutter/painting.dart' show PaintingBinding;
import 'package:flutter/services.dart' show MethodChannel;

import '../native/memory_bindings.dart';
import '../state/app_state.dart';

class MemoryPolicy {
  MemoryPolicy._();

  static const _channel = MethodChannel('dapple/memory');
  static AppState? _app;

  /// Decoded images Flutter keeps for reuse (thumbnails, swatches): 48 MB instead of the default 100 MB. Live (on-screen) images do not count.
  static const imageCacheBytes = 48 << 20;

  // Android ComponentCallbacks2 levels
  static const _runningModerate = 5, _runningLow = 10, _runningCritical = 15, _uiHidden = 20, _background = 40;

  static Future<void> start(AppState app) async {
    _app = app;
    if (kIsWeb) return;
    final c = PaintingBinding.instance.imageCache;
    if (c.maximumSizeBytes > imageCacheBytes) c.maximumSizeBytes = imageCacheBytes;
    if (!Platform.isAndroid) return;
    _channel.setMethodCallHandler((call) async {
      if (call.method == 'trim' && call.arguments is int) await onTrimMemory(call.arguments as int);
      return null;
    });
    try {
      final d = await _channel.invokeMapMethod<String, Object?>('device');
      if (d != null) MemoryBindings.configure(d);
    } catch (e) {
      debugPrint('dapple mem: device info unavailable: $e');
    }
  }

  // ---- the editor --------------------------------------------------------------------------------------------------------------------------------
  static int _editorGen = 0;
  static int _editorsOpen = 0;

  /// An editor was opened (EditorEngine): a pending release for the previous one is called off.
  static void editorOpened() {
    _editorGen++;
    _editorsOpen++;
  }

  /// The editor was closed (EditorEngine.dispose). Its thumbnail commit runs first (commitEdit renders the 2048 px preview on the photo's session);
  /// then, unless an editor opened again, every session is closed and the engine trims its caches.
  static void editorClosed() {
    if (_editorsOpen > 0) _editorsOpen--;
    final gen = ++_editorGen;
    if (_app == null) return; // not started (tests, no engine): nothing to release, and no timer left pending
    Timer(const Duration(seconds: 3), () async {
      final app = _app;
      if (app == null || gen != _editorGen || _editorsOpen > 0) {
        debugPrint('dapple mem: editor closed: release skipped (gen ${gen == _editorGen}, editors open $_editorsOpen)');
        return;
      }
      final t = Stopwatch()..start();
      await _bounded(app.whenCommitsDone(), 'thumbnail commit');
      if (gen != _editorGen || _editorsOpen > 0) return;
      final tc = t.elapsedMilliseconds;
      await _bounded(app.sessions.closeAll(), 'closing the sessions');
      if (gen != _editorGen || _editorsOpen > 0) return;
      final ts = t.elapsedMilliseconds;
      final freed = await MemoryBindings.trim(editorClosedTrimLevel);
      debugPrint('dapple mem: editor closed: commit $tc ms, sessions closed ${ts - tc} ms, engine trim $editorClosedTrimLevel released ${freed >> 20} MB, '
          'total ${t.elapsedMilliseconds} ms');
    });
  }

  /// Engine trim level when the editor has gone: 1 = the caches of level 0 (mosaics, GPU pool, film workspaces) plus every AI model and the
  /// local-adjustment planes (they are rebuilt on the next use: an AI model loads in ~0.1-0.3 s, a mask plane in tens of ms).
  static const editorClosedTrimLevel = 1;

  /// A step of the release that must not hold the trim back forever (a session close waits for its work in flight).
  static Future<void> _bounded(Future<void> f, String what) async {
    try {
      await f.timeout(const Duration(seconds: 15));
    } on TimeoutException {
      debugPrint('dapple mem: editor closed: $what still running after 15 s, trimming anyway');
    } catch (e) {
      debugPrint('dapple mem: editor closed: $what failed: $e');
    }
  }

  // ---- export --------------------------------------------------------------------------------------------------------------------------------------
  /// Before a full-size export of [photoId]: the sessions it does not use go (neighbours, 2x / 1:1 detail; the open photo's own session stays, the
  /// editor is drawing from it). The engine frees its other caches at the FULL open (trim for export).
  static Future<void> beforeExport(String photoId) async {
    final app = _app;
    if (app == null) return;
    await app.sessions.closeAllExcept(photoId);
  }

  // ---- system pressure (Android onTrimMemory) ------------------------------------------------------------------------------------------------------
  static Future<void> onTrimMemory(int level) async {
    final app = _app;
    final c = PaintingBinding.instance.imageCache;
    int engineLevel;
    if (level >= _background || level == _runningCritical) {
      engineLevel = 2;
    } else if (level >= _runningLow) {
      engineLevel = 1; // RUNNING_LOW, UI_HIDDEN
    } else if (level >= _runningModerate) {
      engineLevel = 0;
    } else {
      return;
    }
    if (engineLevel >= 1) c.clear(); // decoded thumbnails not on screen: re-decoded from their files when needed
    if (app != null && (level >= _uiHidden || level == _runningCritical) && _editorsOpen == 0) await app.sessions.closeAll();
    if (app != null && engineLevel >= 1 && _editorsOpen > 0) await app.sessions.closeAllExcept(app.sessions.current);
    final freed = await MemoryBindings.trim(engineLevel);
    debugPrint('dapple mem: onTrimMemory($level) -> engine trim $engineLevel released ${freed >> 20} MB');
  }
}
