import 'package:dapple/services/edit_interaction.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/controls.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('DSlider reports drags to the engine (EditInteraction) and balances on release', (tester) async {
    expect(EditInteraction.depth.value, 0);
    var v = 0.0;
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: Scaffold(
        body: StatefulBuilder(
          builder: (context, set) => DSlider(
            keyPrefix: 't',
            label: 'x',
            value: v,
            min: -100,
            max: 100,
            def: 0,
            display: '$v',
            onChanged: (n) => set(() => v = n),
          ),
        ),
      ),
    ));
    final seen = <int>[];
    EditInteraction.depth.addListener(() => seen.add(EditInteraction.depth.value));
    final g = await tester.startGesture(tester.getCenter(find.byKey(const ValueKey('t-slider'))));
    expect(EditInteraction.depth.value, 1);
    await g.moveBy(const Offset(40, 0));
    await tester.pump();
    expect(EditInteraction.depth.value, 1);
    await g.up();
    expect(EditInteraction.depth.value, 0);
    expect(seen, [1, 0]);

    // removed while dragging: still balanced
    final g2 = await tester.startGesture(tester.getCenter(find.byKey(const ValueKey('t-slider'))));
    expect(EditInteraction.depth.value, 1);
    await tester.pumpWidget(const MaterialApp(home: SizedBox()));
    expect(EditInteraction.depth.value, 0);
    await g2.up();
    expect(EditInteraction.depth.value, 0);
  });
}
