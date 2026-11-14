import 'package:flutter_test/flutter_test.dart';

import '../local_ai_test.dart' show FakeLocalAi;
import '../local_panel_test.dart' show addViaCard, tapKey;
import 'audit_support.dart';

void main() {
  for (final s in ['p', 'l', 'f']) {
    for (final dark in [false, true]) {
      final th = dark ? 'dark' : 'light';
      if (s != 'p' && dark) continue;
      testWidgets('A1 $s $th', (t) async {
        final fake = FakeLocalAi();
        await audOpen(t, s, dark, ai: fake);
        await audShot(t, '01_local_empty_${s}_$th');
        await addViaCard(t, 'radial');
        await tapKey(t, 'local-add');
        await audShot(t, '02_local_addcard_${s}_$th');
        await tapKey(t, 'kind-brush');
        await audShot(t, '04_local_brush_${s}_$th');
        await tapKey(t, 'local-chip-0');
        await tapKey(t, 'local-chip-0');
        await audShot(t, '03_local_actions_${s}_$th');
        await tapKey(t, 'local-add');
        await addViaCard(t, 'luminance');
        await audShot(t, '05_local_tone_${s}_$th');
        await tapKey(t, 'local-add');
        await addViaCard(t, 'color');
        await audShot(t, '06_local_hue_${s}_$th');
      });
    }
  }
}
