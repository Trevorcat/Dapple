import 'package:flutter_test/flutter_test.dart';

import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';

void main() {
  test('single-image HDR: one 0..100 slider in the light group, plain label', () {
    final s = specByKey['hdr']!;
    expect(s.label, '单张 HDR');
    expect(s.group, EGroup.light);
    expect([s.min, s.max, s.def], [0, 100, 0]);
    final it = padItems(PadCat.light).firstWhere((i) => i.id == 'hdr');
    expect(it.label, '单张 HDR');
    expect(it.fmt(60), '60%');
    // not driven by the pad position: the recipe leaves it at 0
    expect(padRecipe(PadCat.light, 0.7, -0.4, 0.5)['hdr'], 0);
  });

  test('single-image HDR: wire format and JSON round trip', () {
    final e = const EditState().withValue('hdr', 60);
    final wire = e.toWire(defaultLook: 'std', defaultToneStrength: 60);
    expect(wire['hdr'], 60);
    final back = EditState.fromJson(e.toJson());
    expect(back.value('hdr'), 60);
    expect(back.sameAs(e), isTrue);
    expect(const EditState().toWire(defaultLook: 'std', defaultToneStrength: 60).containsKey('hdr'), isFalse);
    expect(syncCatOfKey('hdr'), SyncCat.light);
  });
}
