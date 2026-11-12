import 'dart:io';
import 'dart:ui' as ui;

/// True when every pixel of an RGBA buffer is (almost) black.
bool isBlankRgba(List<int> rgba, {int threshold = 3}) {
  for (var i = 0; i + 3 < rgba.length; i += 4) {
    if (rgba[i] > threshold || rgba[i + 1] > threshold || rgba[i + 2] > threshold) return false;
  }
  return true;
}

/// Decodes [path] at a tiny size and reports whether it is entirely black.
/// Returns false when the file is missing or undecodable (not proof of a bad image).
Future<bool> isBlankImageFile(String path) async {
  try {
    final f = File(path);
    if (!await f.exists()) return false;
    final codec = await ui.instantiateImageCodec(await f.readAsBytes(), targetWidth: 48);
    final fr = await codec.getNextFrame();
    final bd = await fr.image.toByteData(format: ui.ImageByteFormat.rawRgba);
    fr.image.dispose();
    codec.dispose();
    if (bd == null) return false;
    return isBlankRgba(bd.buffer.asUint8List());
  } catch (_) {
    return false;
  }
}
