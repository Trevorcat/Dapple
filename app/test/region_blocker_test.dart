// The zoom detail's true-resolution region path: which edits it can render like the full picture (EditorEngine.regionBlocker).
import 'package:dapple/services/editor_engine.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('pixel-only edits use the region path', () {
    expect(EditorEngine.regionBlocker({'exposure': 10, 'crop': 'orig'}), isNull);
    expect(EditorEngine.regionBlocker({}), isNull);
  });

  test('geometry and vignette are named', () {
    expect(EditorEngine.regionBlocker({'crop': '4:3'}), 'crop');
    expect(EditorEngine.regionBlocker({'rot': 1, 'flip': true}), 'rotate / flip');
    expect(EditorEngine.regionBlocker({'vignette': -20}), 'vignette');
  });

  test('local adjustments are named as such, not as crop / vignette', () {
    final local = {
      'v': 1,
      'items': [
        {'id': 'a', 'strength': 100, 'mask': [], 'adjust': {'exposure': 10}},
      ],
    };
    expect(EditorEngine.regionBlocker({'local': local}), 'local adjustments');
  });

  test('switched-off or zero-strength local adjustments do not block the region path', () {
    final local = {
      'v': 1,
      'items': [
        {'id': 'a', 'on': false, 'strength': 100, 'mask': [], 'adjust': {'exposure': 10}},
        {'id': 'b', 'strength': 0, 'mask': [], 'adjust': {'exposure': 10}},
      ],
    };
    expect(EditorEngine.regionBlocker({'local': local}), isNull);
  });
}
