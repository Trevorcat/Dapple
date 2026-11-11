import 'package:flutter/services.dart';
import 'package:share_plus/share_plus.dart';

/// Hand-off to other apps. Capability ladder (requirements 4.11.3): multi-image share sheet,
/// single-image share, save to gallery "Dapple" then open the target app.
abstract class ShareService {
  /// Replaceable in tests.
  static ShareService instance = AndroidShareService();

  /// System share sheet (ACTION_SEND_MULTIPLE for several files).
  /// Returns false when the user dismissed the sheet without choosing a target.
  Future<bool> shareFiles(List<String> paths);

  /// Launches [package]; false when it is not installed / cannot be opened.
  Future<bool> openApp(String package);
}

class AndroidShareService implements ShareService {
  static const _ch = MethodChannel('dapple/media');

  @override
  Future<bool> shareFiles(List<String> paths) async {
    final files = [for (final p in paths) XFile(p, mimeType: 'image/jpeg')];
    final r = await SharePlus.instance.share(ShareParams(files: files));
    return r.status != ShareResultStatus.dismissed;
  }

  @override
  Future<bool> openApp(String package) async {
    try {
      return (await _ch.invokeMethod<bool>('openApp', {'package': package})) ?? false;
    } catch (_) {
      return false;
    }
  }
}
