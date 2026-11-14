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

  test('local adjustments use the region path (the window carries its parent photo)', () {
    final local = {
      'v': 1,
      'items': [
        {'id': 'a', 'strength': 100, 'mask': [], 'adjust': {'exposure': 10}},
      ],
    };
    expect(EditorEngine.regionBlocker({'local': local}), isNull);
  });

  test('local adjustments on a film frame are named, and keep the half-resolution detail', () {
    final local = {
      'v': 1,
      'items': [
        {'id': 'a', 'strength': 100, 'mask': [], 'adjust': {'exposure': 10}},
      ],
    };
    expect(EditorEngine.regionBlocker({'local': local, 'film': 'kodak_portra_400'}), 'local adjustments on a film frame');
    expect(EditorEngine.regionBlocker({'film': 'kodak_portra_400'}), isNull);
    expect(EditorEngine.regionBlocker({'local': local, 'crop': '4:3'}), 'crop');
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
