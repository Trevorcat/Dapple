import 'dart:io';

import 'package:flutter/services.dart';

/// Test-run trigger files (Documents/DAPPLE_OPEN, Documents/autotest.json) are one-shot: a launch reads one, deletes it, and only a
/// launch that actually consumed one counts as a test run. A leftover from an earlier session (older than [maxAge]) is deleted unread,
/// so a later normal launch is a normal app (the system Photos prompt must never be skipped because of a stale file).
class TestTrigger {
  static const maxAge = Duration(minutes: 10);
  static const MethodChannel _photos = MethodChannel('dapple/photos');

  /// The file's text when it exists and is fresh; the file is deleted in both cases. Null when absent or stale.
  static String? consume(File f, {DateTime? now, Duration age = maxAge}) {
    try {
      if (!f.existsSync()) return null;
      final fresh = (now ?? DateTime.now()).difference(f.lastModifiedSync()) <= age;
      final text = fresh ? f.readAsStringSync() : null;
      f.deleteSync();
      return text;
    } catch (_) {
      return null;
    }
  }

  /// Tells the iOS plugin that a test run is in progress in this launch (nobody can answer the Photos prompt) or that it ended.
  static Future<void> setActive(bool on) async {
    try {
      await _photos.invokeMethod<void>('testRun', {'active': on});
    } catch (_) {}
  }
}
