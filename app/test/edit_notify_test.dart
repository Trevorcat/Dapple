import 'package:dapple/state/app_state.dart';
import 'package:flutter_test/flutter_test.dart';

/// A drag step must not rebuild the library / shell / top bar (they sit under the editor route and cost a
/// low-end phone 10-25 ms per pointer move); the editor follows [AppState.editTick] instead.
void main() {
  test('gesture edits bump editTick without notifying AppState listeners (after the first one)', () {
    final app = AppState.demo();
    final id = app.editable.first.id;
    var notified = 0, ticks = 0;
    app.addListener(() => notified++);
    app.editTick.addListener(() => ticks++);

    app.applyEdit(id, (e) => e.withValue('exposure', 10), coalesce: 'g#1', gesture: true);
    app.applyEdit(id, (e) => e.withValue('exposure', 20), coalesce: 'g#1', gesture: true);
    app.applyEdit(id, (e) => e.withValue('exposure', 30), coalesce: 'g#1', gesture: true);
    expect(ticks, 3);
    expect(notified, lessThanOrEqualTo(1)); // at most the step that flips "exported" off
    expect(app.editOf(id).value('exposure'), 30);

    final before = notified;
    app.applyEdit(id, (e) => e.withValue('contrast', 5)); // a discrete edit still notifies everyone
    expect(notified, before + 1);
    expect(ticks, 4);

    app.undo(id);
    expect(ticks, 5);
  });
}
