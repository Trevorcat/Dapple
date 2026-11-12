import 'package:dapple/services/live_bridge.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('EditedClip serialises what the Kotlin EditSpec reads', () {
    const e = EditedClip(paramsJson: '{"a":1}', refMs: 1200, stillAspect: 1.5, stillLongEdge: 4000, sceneX: 0.31, sceneY: 0.33, cameraMake: 'SONY');
    final m = e.toMap();
    expect(m['paramsJson'], '{"a":1}');
    expect(m['lutPath'], '');
    expect(m['refMs'], 1200);
    expect(m['stillAspect'], 1.5);
    expect(m['stillLongEdge'], 4000);
    expect(m['cameraMake'], 'SONY');
    expect(m.keys.toSet(), {'paramsJson', 'lutPath', 'refMs', 'stillAspect', 'stillLongEdge', 'sceneX', 'sceneY', 'cameraMake', 'cameraModel'});
  });
}
