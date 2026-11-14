// The 局部 add-area view on an iPhone (docs/ios/device-results.md round 5: a large empty block between the area chips and the tool tiles):
import 'dart:ui' show Size;
// the tiles follow the chips with the normal spacing, whatever height the panel has.
import 'package:flutter_test/flutter_test.dart';

import 'local_panel_test.dart' show addViaCard, key, openLocal, tapKey;

void main() {
  // iPhone X / 11 Pro (375 x 812, 3x, top 47 / bottom 34) and XR / 11 (414 x 896, 2x, top 48 / bottom 34)
  for (final (name, phys, dpr, top) in const [('375x812', Size(1125, 2436), 3.0, 47.0), ('414x896', Size(828, 1792), 2.0, 48.0)]) {
    testWidgets('add-area view at $name with iOS insets: tiles follow the chips row', (t) async {
      final r = await openLocal(t, phys: phys, dpr: dpr, top: top, bottom: 34);
      await addViaCard(t, 'radial');
      await tapKey(t, 'local-add');
      expect(key('kind-linear'), findsOneWidget);
      final chips = t.getRect(key('local-add'));
      // the first group title hangs under the chips (the tiles follow their title)
      final title = t.getRect(key('kind-group-shape'));
      final gap = title.top - chips.bottom;
      expect(gap, greaterThanOrEqualTo(0));
      expect(gap, lessThanOrEqualTo(12), reason: 'normal spacing, not an empty block');
      expect(t.getRect(key('kind-linear')).top - title.bottom, lessThanOrEqualTo(8));
      expect(r.lc.list.length, 1);
      expect(t.takeException(), isNull);
    });
  }
}
