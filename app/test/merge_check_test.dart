// The plausibility check before HDR merge / panorama stitch, on synthetic frames: a real bracket, a hand-held bracket, panorama tiles
// picked for HDR, unrelated scenes, a real panorama, one framing picked for a panorama, tiles with a gap.
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:dapple/services/merge_check.dart';
import 'package:flutter_test/flutter_test.dart';

const _w = 64, _h = 48;

/// A wide synthetic street scene, [width] x 48: a sky gradient over a dark ground, and many rectangles (windows, poles, roofs).
Float32List _scene(int seed, {int width = 420}) {
  final rnd = math.Random(seed);
  final s = Float32List(width * _h);
  for (var y = 0; y < _h; y++) {
    for (var x = 0; x < width; x++) {
      s[y * width + x] = y < 20 ? 0.75 - y * 0.01 : 0.25;
    }
  }
  for (var i = 0; i < width ~/ 2; i++) {
    final x0 = rnd.nextInt(width - 6), y0 = rnd.nextInt(_h - 6);
    final rw = 2 + rnd.nextInt(10), rh = 2 + rnd.nextInt(14);
    final v = rnd.nextDouble();
    for (var y = y0; y < math.min(_h, y0 + rh); y++) {
      for (var x = x0; x < math.min(width, x0 + rw); x++) {
        s[y * width + x] = v;
      }
    }
  }
  return s;
}

/// A [_w] x [_h] window of [scene] at column [x0], exposure [ev] stops (clipped), a sensor-ish noise of [noise].
FrameProbe _crop(Float32List scene, int width, int x0, {double ev = 0, double? evTag, int dy = 0, double noise = 0.004, int seed = 1}) {
  final rnd = math.Random(seed);
  final l = Float32List(_w * _h);
  final gain = math.pow(2, ev).toDouble();
  for (var y = 0; y < _h; y++) {
    for (var x = 0; x < _w; x++) {
      final sy = (y + dy).clamp(0, _h - 1);
      final lin = math.pow(scene[sy * width + (x0 + x)], 2.2) * gain;
      final v = math.pow(lin.clamp(0.0, 1.0), 1 / 2.2).toDouble() + (rnd.nextDouble() - 0.5) * noise;
      l[y * _w + x] = v.clamp(0.0, 1.0);
    }
  }
  return FrameProbe(_w, _h, l, ev: evTag);
}

void main() {
  final a = _scene(7), b = _scene(99);

  group('HDR bracket', () {
    test('a real three-frame bracket passes (EV from EXIF)', () {
      final f = [for (final ev in [-2.0, 0.0, 2.0]) _crop(a, 420, 100, ev: ev, evTag: ev)];
      expect(checkBracket(f).isOk, isTrue);
    });

    test('a hand-held bracket (1-2 px shift, no EXIF) passes', () {
      final f = [
        _crop(a, 420, 100, ev: -2, dy: 0),
        _crop(a, 420, 101, ev: 0, dy: 1),
        _crop(a, 420, 99, ev: 2, dy: -1),
      ];
      expect(checkBracket(f).isOk, isTrue);
    });

    test('panorama tiles picked for HDR: not the same scene', () {
      final f = [for (final x in [0, 40, 80]) _crop(a, 420, x)];
      expect(checkBracket(f).problem, MergeProblem.notSameScene);
    });

    test('tiles of a horizon scene (same sky gradient in every one) are still not a bracket', () {
      final f = [for (final x in [0, 45, 90, 135]) _crop(a, 420, x)];
      expect(checkBracket(f).problem, MergeProblem.notSameScene);
    });

    test('two unrelated scenes', () {
      final r = checkBracket([_crop(a, 420, 100, ev: -1), _crop(b, 420, 100, ev: 1)]);
      expect(r.problem, MergeProblem.notSameScene);
      expect(r.message, isNotEmpty);
    });

    test('the same scene at the same exposure (EXIF known): no spread', () {
      final f = [for (var i = 0; i < 3; i++) _crop(a, 420, 100, evTag: 0, seed: i)];
      expect(checkBracket(f).problem, MergeProblem.sameExposure);
    });

    test('the same scene, no EXIF, measured brightness identical: no spread', () {
      final f = [for (var i = 0; i < 3; i++) _crop(a, 420, 100, seed: i)];
      expect(checkBracket(f).problem, MergeProblem.sameExposure);
    });

    test('frames without a preview and a single frame never block', () {
      expect(checkBracket([null, null, null]).isOk, isTrue);
      expect(checkBracket([_crop(a, 420, 0)]).isOk, isTrue);
      expect(checkBracket([_crop(a, 420, 100, ev: -2), null, _crop(a, 420, 100, ev: 2)]).isOk, isTrue);
    });

    test('flat pictures (no detail to compare) do not raise an alarm', () {
      FrameProbe flat(double v) => FrameProbe(_w, _h, Float32List.fromList(List.filled(_w * _h, v)), ev: v * 4);
      expect(checkBracket([flat(0.2), flat(0.5), flat(0.8)]).isOk, isTrue);
    });

    test('different sizes are not a bracket', () {
      final small = FrameProbe(32, 24, Float32List(32 * 24));
      expect(checkBracket([_crop(a, 420, 0, ev: -1), small]).problem, MergeProblem.notSameScene);
    });
  });

  group('panorama', () {
    test('overlapping tiles (35 % overlap) pass', () {
      final f = [for (final x in [0, 42, 84, 126]) _crop(a, 420, x, seed: x)];
      expect(checkPanorama(f).isOk, isTrue);
    });

    test('hand-held tiles with a small vertical drift pass', () {
      final f = [_crop(a, 420, 0), _crop(a, 420, 44, dy: 2), _crop(a, 420, 88, dy: -1)];
      expect(checkPanorama(f).isOk, isTrue);
    });

    test('one framing picked for a panorama: same framing', () {
      final f = [for (var i = 0; i < 3; i++) _crop(a, 420, 100, ev: i - 1.0, seed: i)];
      expect(checkPanorama(f).problem, MergeProblem.sameFraming);
    });

    test('tiles with a gap between them: no overlap', () {
      final f = [_crop(a, 420, 0), _crop(a, 420, 160), _crop(a, 420, 320)];
      expect(checkPanorama(f).problem, MergeProblem.noOverlap);
    });

    test('unrelated scenes: no overlap', () {
      expect(checkPanorama([_crop(a, 420, 50), _crop(b, 420, 90)]).problem, MergeProblem.noOverlap);
    });

    test('missing previews never block', () {
      expect(checkPanorama([null, _crop(a, 420, 0), null]).isOk, isTrue);
    });
  });

  test('checkFrames picks the check by the engine kind', () {
    final tiles = [for (final x in [0, 42, 84]) _crop(a, 420, x)];
    expect(checkFrames('hdr', tiles).isOk, isFalse);
    expect(checkFrames('pano', tiles).isOk, isTrue);
  });
}
