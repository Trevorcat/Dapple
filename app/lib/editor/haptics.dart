// Landmark-only haptics (requirements 4.15.9, guidelines 15.4). Continuous value changes, zoom,
// long press and readouts stay silent. Sent through View.performHapticFeedback on Android (the
// system "touch feedback" switch is honoured by the OS); the in-app switch is [enabled].
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'value_mapping.dart';

enum HapticKind { autoSnap, detent, endStop }

class Haptics {
  Haptics({this.enabled = true, this.onFire});

  static const MethodChannel _channel = MethodChannel('dapple/haptics');
  bool enabled;

  /// Test hook: called with every haptic that is actually emitted.
  final void Function(HapticKind)? onFire;

  final Stopwatch _clock = Stopwatch()..start();
  int _lastMs = -1000;

  static HapticKind? kindOf(Landmark l) => switch (l) {
        Landmark.none => null,
        Landmark.autoSnap => HapticKind.autoSnap,
        Landmark.detent => HapticKind.detent,
        Landmark.endStop => HapticKind.endStop,
      };

  /// Emits the haptic of [l] (no-op for [Landmark.none]). Two haptics are never closer than 40 ms.
  void landmark(Landmark l) {
    final k = kindOf(l);
    if (k != null) fire(k);
  }

  void fire(HapticKind kind) {
    if (!enabled) return;
    final now = _clock.elapsedMilliseconds;
    if (now - _lastMs < 40) return;
    _lastMs = now;
    onFire?.call(kind);
    if (kIsWeb) return;
    try {
      if (Platform.isAndroid) {
        _channel.invokeMethod<void>('perform', kind.name).catchError((Object _) {});
      } else if (Platform.isIOS) {
        switch (kind) {
          case HapticKind.autoSnap:
            HapticFeedback.lightImpact();
          case HapticKind.detent:
            HapticFeedback.selectionClick();
          case HapticKind.endStop:
            HapticFeedback.heavyImpact();
        }
      }
    } catch (_) {
      // no platform (tests): silently ignore
    }
  }
}
