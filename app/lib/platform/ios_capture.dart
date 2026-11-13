// iOS camera capture: the Swift plugin (DappleCapturePlugin.swift, AVFoundation) on the same channel and result maps as Android's CapturePlugin.kt
// (`capture` -> {jpg?, dng?, thumb?, source}), so capture_flow.dart and AppState.ingestCapture are shared. The capture screen itself is native
// (full-screen view controller with an AVCaptureVideoPreviewLayer), presented by the `capture` call.
//
// Extra calls against Android: `available`, `permission` (camera permission_handler is not linked on iOS), `openSettings`, `capabilities`
// (per lens: RAW kind proraw / bayer / none, maximum pixels, mode ladder), `autoShutter` (test runs).
import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import '../services/capture.dart';
import '../services/ingest.dart';

class IosCaptureBackend implements CaptureBackend {
  /// [probe] asks the plugin whether a camera exists (false on the simulator) right away; tests pass false and set [debugAvailable].
  IosCaptureBackend({MethodChannel? channel, bool probe = true}) : _ch = channel ?? const MethodChannel('dapple/capture') {
    if (probe) refreshAvailability();
  }

  final MethodChannel _ch;
  bool _available = false;

  /// Test runs set this to false: the shots must not land in the owner's Photos library (the Android-style gallery copy).
  static bool saveToLibrary = true;

  @override
  bool get available => _available;

  @visibleForTesting
  set debugAvailable(bool v) => _available = v;

  /// The home screen reads [available] synchronously; the answer of the plugin arrives within the first frames.
  Future<bool> refreshAvailability() async {
    try {
      _available = (await _ch.invokeMethod<bool>('available')) == true;
    } catch (_) {
      _available = false; // no handler (tests, simulator without the plugin): the camera button simply does not show
    }
    return _available;
  }

  @override
  Future<CapturePerm> permission({bool request = false}) async {
    try {
      switch (await _ch.invokeMethod<String>('permission', {'request': request})) {
        case 'granted':
          return CapturePerm.granted;
        case 'blocked':
          return CapturePerm.blocked;
      }
    } catch (_) {}
    return CapturePerm.denied;
  }

  @override
  Future<bool> openSettings() async {
    try {
      return (await _ch.invokeMethod<bool>('openSettings')) == true;
    } catch (_) {
      return false;
    }
  }

  @override
  Future<CaptureResult?> capture() async {
    final r = await _ch.invokeMapMethod<String, dynamic>('capture');
    final jpg = r?['jpg'] as String?;
    final dng = r?['dng'] as String?;
    if (r == null || (jpg == null && dng == null)) return null;
    return CaptureResult(jpegPath: jpg, dngPath: dng, thumbPath: r['thumb'] as String?, source: (r['source'] as String?) ?? 'avfoundation');
  }

  /// What every lens of this phone really offers (RAW kind, pixels, mode ladder); lenses are empty until the camera permission is granted.
  Future<Map<String, dynamic>> capabilities() async {
    try {
      return Map<String, dynamic>.from(await _ch.invokeMethod<Map<Object?, Object?>>('capabilities') ?? const {});
    } catch (_) {
      return const {};
    }
  }

  /// Test runs: the shutter presses itself [delayMs] after the screen opened.
  Future<void> setAutoShutter(bool on, {int delayMs = 5000, String? mode}) async {
    try {
      await _ch.invokeMethod<void>('autoShutter', {'on': on, 'delayMs': delayMs, 'mode': ?mode});
    } catch (_) {}
  }

  @override
  Future<void> saveToGallery(String path, String name, String mime) async {
    if (!Platform.isIOS || !saveToLibrary) return;
    await MediaBridge.saveToGallery(path, name);
  }
}
