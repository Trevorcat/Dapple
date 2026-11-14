import 'package:dapple/screens/editor/local_controller.dart' show LocalBody;
import 'package:flutter_test/flutter_test.dart';

import '../editor_v2_test.dart' show settle;
import '../local_ai_test.dart' show FakeLocalAi;
import '../local_panel_test.dart' show tapKey;
import '../local_portrait_test.dart' show portraitResult;
import 'audit_support.dart';

void main() {
  for (final s in ['p', 'l', 'f']) {
    for (final dark in [false, true]) {
      final th = dark ? 'dark' : 'light';
      if (s != 'p' && dark) continue;
      testWidgets('A2 $s $th', (t) async {
        final fake = FakeLocalAi();
        fake.setFace(1, iod: 0.125);
        final r = await audOpen(t, s, dark, ai: fake);
        await tapKey(t, 'kind-portrait');
        await audShot(t, '10_portrait_analysing_${s}_$th');
        fake.finish(portraitResult());
        await settle(t, 4);
        await audShot(t, '11_portrait_chips_${s}_$th');
        r.lc!.showBody(LocalBody.pads);
        await settle(t, 3);
        await audShot(t, '12_portrait_skin_heal_${s}_$th');
        await tapKey(t, 'heal-mode');
        await audShot(t, '13_portrait_healmode_${s}_$th');
        await tapKey(t, 'heal-mode');
        await tapKey(t, 'items-button');
        await audShot(t, '14_portrait_items_${s}_$th');
      });
    }
    // a portrait-format photo beside the pane (audit F25): the photo takes the full height, the top bar stands as a rail
    if (s != 'p') {
      testWidgets('A2 $s tall photo', (t) async {
        final fake = FakeLocalAi();
        fake.setFace(1, iod: 0.125);
        await audOpen(t, s, false, ai: fake, title: '街角咖啡');
        await tapKey(t, 'kind-portrait');
        fake.finish(portraitResult());
        await settle(t, 4);
        await audShot(t, '15_portrait_tall_photo_${s}_light');
      });
    }
  }
}
