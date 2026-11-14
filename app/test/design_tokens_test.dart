// Design-token lint (guidelines 2.1 / 3 / 5, audit 2026-11 F29, F30): the surfaces the audit covered take colours, radii and type
// from the token set (`context.c`, `Fx`, `Rd`, `T`, `Mo`), not from literals. A literal colour that is not a colour (an alpha mask) is
// marked on its line with `// token-exempt: <why>`.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

const _files = [
  'lib/screens/editor/local_panel.dart',
  'lib/screens/editor/pad_panel.dart',
  'lib/screens/editor/pad_dock.dart',
  'lib/screens/editor/film_map_view.dart',
  'lib/screens/home/photo_tile.dart',
  'lib/screens/home/usb_picker_page.dart',
  'lib/screens/home/conn_card.dart',
  'lib/screens/home/import_flow.dart',
  'lib/screens/publish/publish_page.dart',
  'lib/screens/library/styles_page.dart',
];

void main() {
  test('no literal colours, no type below 11 px in the audited surfaces', () {
    final bad = <String>[];
    final colour = RegExp(r'Color\(0x|Colors\.(?!transparent)');
    final size = RegExp(r'fontSize:\s*([0-9.]+)');
    for (final f in _files) {
      final lines = File(f).readAsLinesSync();
      for (var i = 0; i < lines.length; i++) {
        final l = lines[i];
        if (l.trimLeft().startsWith('//') || l.contains('token-exempt')) continue;
        if (colour.hasMatch(l)) bad.add('$f:${i + 1}: literal colour: ${l.trim()}');
        for (final m in size.allMatches(l)) {
          if (double.parse(m.group(1)!) < 11) bad.add('$f:${i + 1}: font size below 11: ${l.trim()}');
        }
      }
    }
    expect(bad, isEmpty, reason: bad.join('\n'));
  });
}
