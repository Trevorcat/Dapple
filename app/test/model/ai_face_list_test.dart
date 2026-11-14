// The AI job result's face list (engine faces meta): parsed additively; an older result without it parses to an empty list.
import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/native/ai_bindings.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('face_list: index, box, yaw, frontal, mouth_open, teeth, iod, center, planes', () {
    final r = AiJobResult.fromJson({
      'state': 'done',
      'faces': 2,
      'face_list': [
        {'index': 1, 'box': [0.1, 0.2, 0.5, 0.7], 'yaw': -4.5, 'frontal': true, 'mouth_open': 0.42, 'teeth': true, 'iod': 0.0931, 'center': [0.3, 0.45], 'planes': ['face', 'lips', 'teeth', 'mouth']},
        {'index': 2, 'box': [0.6, 0.2, 0.8, 0.5], 'yaw': 38.0, 'frontal': false, 'mouth_open': 0.0, 'teeth': false, 'iod': 0.031, 'center': [0.7, 0.35], 'planes': ['face']},
      ],
    });
    expect(r.faceList, hasLength(2));
    final f = r.faceList.first;
    expect(f.index, 1);
    expect(f.box, [0.1, 0.2, 0.5, 0.7]);
    expect(f.yaw, -4.5);
    expect(f.frontal, isTrue);
    expect(f.mouthOpen, 0.42);
    expect(f.teeth, isTrue);
    expect(f.iod, 0.0931);
    expect(f.center, [0.3, 0.45]);
    expect(f.planes, contains('mouth'));
    expect(r.faceList[1].frontal, isFalse);
  });

  test('an older result (no face_list) parses to an empty list', () {
    expect(AiJobResult.fromJson({'state': 'done', 'faces': 1}).faceList, isEmpty);
  });

  test('luminance components: feather 15 when absent (the engine default), refine kept and written, unknown keys preserved', () {
    final c = MaskComponent.fromJson({'kind': 'luminance', 'op': 'add', 'lo': 20, 'hi': 60, 'future': 3})!;
    expect(c.lumFeather, 15);
    expect(c.refine, 50);
    final j = c.toJson();
    expect(j['feather'], 15);
    expect(j['refine'], 50);
    expect(j['future'], 3, reason: 'unknown keys survive');
    final d = MaskComponent.fromJson({'kind': 'luminance', 'lo': 20, 'hi': 60, 'feather': 8, 'refine': 0})!;
    expect(d.toJson()['refine'], 0);
    expect(d.toJson()['feather'], 8);
  });
}
