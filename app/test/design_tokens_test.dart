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

  test('no Material icons in the editor: the app draws its own line set (guidelines 8, audit F10)', () {
    final bad = <String>[];
    for (final f in Directory('lib/screens/editor').listSync().whereType<File>().where((f) => f.path.endsWith('.dart'))) {
      final lines = f.readAsLinesSync();
      for (var i = 0; i < lines.length; i++) {
        if (lines[i].contains('Icons.')) bad.add('${f.path}:${i + 1}: ${lines[i].trim()}');
      }
    }
    expect(bad, isEmpty, reason: bad.join('\n'));
  });

  test('every panel header is the one header (U6): no header built by hand in the editor', () {
    // PanelHeader asserts at most three buttons; the category and 局部 headers must go through it
    for (final f in ['lib/screens/editor/pad_panel.dart', 'lib/screens/editor/local_panel.dart']) {
      final s = File(f).readAsStringSync();
      expect(s.contains('PanelHeader('), isTrue, reason: f);
      expect(RegExp(r"HeaderButton\([^;]*?label: '返回'").hasMatch(s), isFalse, reason: '$f: back is the ‹ of the header, not a button (U6)');
      expect(s.contains("'对比'"), isFalse, reason: '$f: no compare button (U4)');
    }
  });
}
