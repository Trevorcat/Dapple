import 'dart:convert';

import 'package:dapple/services/live_edit.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('an unedited Motion Photo stays pass-through', () {
    expect(liveClipEdited(userEdited: false, autoCorrectionApplied: false), isFalse);
    expect(liveClipEdited(userEdited: false, autoCorrectionApplied: false, cropOverride: 'orig'), isFalse);
    expect(liveClipEdited(userEdited: false, autoCorrectionApplied: false, cropOverride: ''), isFalse);
    expect(editedClipFor(wire: {'look': 'std'}, edited: false, coverMs: 1500), isNull);
  });

  test('an edited photo exports its clip with the still\'s edit, film included', () {
    for (final c in [
      liveClipEdited(userEdited: true, autoCorrectionApplied: false),
      liveClipEdited(userEdited: false, autoCorrectionApplied: true),
      liveClipEdited(userEdited: false, autoCorrectionApplied: false, cropOverride: '4:5'),
    ]) {
      expect(c, isTrue);
    }
    final wire = {'film': 'kodak_portra_400', 'exposure': 0.3, 'look': 'std'};
    final e = editedClipFor(wire: wire, edited: true, coverMs: 6000, stillLongEdge: 4000)!;
    expect(jsonDecode(e.paramsJson), wire);
    expect(e.refMs, 6000);
    expect(e.stillLongEdge, 4000);
    final m = e.toMap();
    expect(m['paramsJson'], e.paramsJson);
    expect(m['refMs'], 6000);
  });
}
