import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

const MethodChannel _log = MethodChannel('dapple/log');

/// One line per photo outcome in the platform log (NSLog on iOS, logcat tag DappleIngest on Android), also in release builds.
/// Only file names, never paths: any path inside [msg] is cut down to its last segment.
void ingestLog(String msg) {
  final line = redactPaths(msg);
  debugPrint('dapple ingest: $line');
  _log.invokeMethod<void>('line', line).catchError((_) {}); // no channel in tests / on desktop
}

/// "failed /var/mobile/Containers/Data/x/Documents/inbox/IMG_1.dng: ..." -> "failed IMG_1.dng: ...".
String redactPaths(String s) => s.replaceAllMapped(RegExp(r'(?:[A-Za-z]:)?(?:[\/][^\/\s:;,]+)+[\/]([^\/\s:;,]+)'), (m) => m[1]!);
