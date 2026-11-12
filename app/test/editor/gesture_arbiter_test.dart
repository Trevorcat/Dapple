import 'package:dapple/editor/gesture_arbiter.dart';
import 'package:flutter_test/flutter_test.dart';

/// Records every intent the arbiter emits.
class Rec implements GestureSink {
  @override
  ArbiterMode mode = ArbiterMode.adjust;
  @override
  ActiveKind activeKind = ActiveKind.oneD;
  @override
  bool zoomed = false;
  @override
  bool adjustEnabled = true;

  final List<String> log = [];
  final List<StrokeSample> strokes = [];
  double lastOffX = 0, lastOffY = 0;
  double sumDx = 0;
  LockAxis axis = LockAxis.none;
  bool lastEndByMulti = false;

  bool has(String prefix) => log.any((e) => e.startsWith(prefix));
  int count(String prefix) => log.where((e) => e.startsWith(prefix)).length;

  @override
  void adjustBegin(LockAxis a, double x, double y) {
    axis = a;
    log.add('adjustBegin:${a.name}');
  }

  @override
  void adjustMove(double dx, double dy, double offX, double offY, double vx, double vy, double dtMs) {
    lastOffX = offX;
    lastOffY = offY;
    sumDx += dx;
    log.add('adjustMove');
  }

  @override
  void adjustEnd({required bool byMultiTouch}) {
    lastEndByMulti = byMultiTouch;
    log.add('adjustEnd:$byMultiTouch');
  }

  @override
  void panBy(double dx, double dy) => log.add('panBy');
  @override
  void panEnd(double vx, double vy) => log.add('panEnd');
  @override
  void pageDrag(double dx) => log.add('pageDrag');
  @override
  void pageEnd(double totalDx, double vx) => log.add('pageEnd');
  @override
  void pageCancel() => log.add('pageCancel');
  @override
  void longPress(bool on) => log.add('longPress:$on');
  @override
  void scaleStart(double fx, double fy) => log.add('scaleStart');
  @override
  void scaleUpdate(double fx, double fy, double scale) => log.add('scaleUpdate:${scale.toStringAsFixed(2)}');
  @override
  void scaleEnd(double vx, double vy) => log.add('scaleEnd');
  @override
  void tap(double x, double y) => log.add('tap');
  @override
  void doubleTap(double x, double y) => log.add('doubleTap');
  @override
  void strokeEnded(StrokeSample s) => strokes.add(s);
  @override
  void pointerActivity({required bool down}) {}
}

void main() {
  late Rec r;
  late GestureArbiter a;
  setUp(() {
    r = Rec();
    a = GestureArbiter(r);
  });

  /// Moves finger [id] to (x, y), 16 ms later each call.
  int t = 0;
  void down(int id, double x, double y) {
    t = 0;
    a.pointerDown(id, x, y, t);
  }

  void move(int id, double x, double y, {int dt = 16}) {
    t += dt;
    a.pointerMove(id, x, y, t);
  }

  void up(int id, double x, double y, {int dt = 16}) {
    t += dt;
    a.pointerUp(id, x, y, t);
  }

  group('axis lock (adjust mode, 1D)', () {
    test('no value change below 10 dp of movement', () {
      down(1, 100, 100);
      move(1, 105, 103);
      move(1, 108, 104);
      expect(r.log, isEmpty);
      expect(a.state, ArbiterState.pending);
    });

    test('theta < 35 deg locks horizontal; the lock point is the reference, vertical wobble is only reported as offset', () {
      down(1, 100, 100);
      move(1, 106, 102);
      move(1, 112, 103); // 12.4 dp, 14 deg
      expect(r.axis, LockAxis.horizontal);
      expect(a.state, ArbiterState.adjustH);
      move(1, 140, 190); // big vertical offset afterwards: still horizontal
      expect(a.state, ArbiterState.adjustH);
      expect(r.lastOffX, closeTo(28, 1e-9)); // from the lock point (112, 103)
      expect(r.lastOffY, closeTo(87, 1e-9));
      expect(r.has('adjustBegin:horizontal'), isTrue);
      expect(r.count('adjustBegin'), 1);
    });

    test('theta > 55 deg locks vertical and horizontal travel is then ignored by the mode (reported only as offset)', () {
      down(1, 100, 100);
      move(1, 103, 108);
      move(1, 104, 112);
      expect(r.axis, LockAxis.vertical);
      expect(a.state, ArbiterState.adjustV);
      move(1, 200, 150);
      expect(a.state, ArbiterState.adjustV);
    });

    test('35-55 deg keeps watching until 16 dp, then the larger component wins', () {
      down(1, 100, 100);
      move(1, 108, 108); // 11.3 dp at 45 deg: undecided
      expect(a.state, ArbiterState.pending);
      expect(r.has('adjustBegin'), isFalse);
      move(1, 112, 116); // 20 dp, dy larger
      expect(r.axis, LockAxis.vertical);
    });

    test('a lock never changes mid gesture', () {
      down(1, 0, 0);
      move(1, 12, 0);
      for (var i = 0; i < 10; i++) {
        move(1, 12 + i * 2.0, i * 30.0);
      }
      expect(r.count('adjustBegin'), 1);
      expect(r.axis, LockAxis.horizontal);
    });
  });

  group('2D active parameter', () {
    test('no axis lock: any direction after 10 dp starts the adjust', () {
      r.activeKind = ActiveKind.twoD;
      down(1, 100, 100);
      move(1, 106, 109); // diagonal
      expect(r.axis, LockAxis.none);
      expect(a.state, ArbiterState.adjust2D);
      expect(r.has('adjustBegin:none'), isTrue);
    });
  });

  group('long press vs drag', () {
    test('stationary 380 ms = compare; fingers moving < 10 dp do not cancel it', () {
      down(1, 50, 50);
      move(1, 55, 54, dt: 100); // 6.4 dp
      a.tick(379);
      expect(r.has('longPress'), isFalse);
      a.tick(380);
      expect(r.log, contains('longPress:true'));
      move(1, 200, 200); // moving afterwards neither adjusts nor pans
      expect(r.has('adjustBegin'), isFalse);
      expect(r.has('panBy'), isFalse);
      up(1, 200, 200);
      expect(r.log.last, 'longPress:false');
      expect(a.state, ArbiterState.idle);
    });

    test('a drag of >= 10 dp before 380 ms cancels the long press for good', () {
      down(1, 50, 50);
      move(1, 63, 50, dt: 100);
      a.tick(500);
      expect(r.has('longPress'), isFalse);
      expect(r.axis, LockAxis.horizontal);
    });

    test('the deadline is exposed so the surface can arm one timer', () {
      down(1, 0, 0);
      expect(a.nextDeadline, 380);
      move(1, 20, 0);
      expect(a.nextDeadline, isNull);
    });
  });

  group('taps', () {
    test('single tap is delivered only after the 280 ms double tap window', () {
      down(1, 10, 10);
      up(1, 10, 10);
      expect(r.has('tap'), isFalse);
      expect(a.nextDeadline, 16 + 280);
      a.tick(295);
      expect(r.has('tap'), isFalse);
      a.tick(296);
      expect(r.count('tap'), 1);
    });

    test('double tap within 280 ms and 40 dp cancels the single tap', () {
      down(1, 10, 10);
      up(1, 10, 10);
      t = 100;
      a.pointerDown(1, 20, 14, t);
      a.pointerUp(1, 20, 14, t + 16);
      expect(r.log, ['doubleTap']);
      a.tick(1000);
      expect(r.has('tap'), isFalse);
    });

    test('second tap too far away or too late is two single taps', () {
      down(1, 10, 10);
      up(1, 10, 10);
      a.tick(400);
      expect(r.count('tap'), 1);
      a.pointerDown(1, 10, 10, 410);
      a.pointerUp(1, 10, 10, 420);
      a.tick(1000);
      expect(r.count('tap'), 2);
      expect(r.has('doubleTap'), isFalse);
    });

    test('adjust never waits for the double tap timeout: a drag right after a tap delivers the tap first, then adjusts', () {
      down(1, 10, 10);
      up(1, 10, 10);
      a.pointerDown(1, 10, 10, 100);
      a.pointerMove(1, 30, 10, 120);
      expect(r.log.take(2), ['tap', 'adjustBegin:horizontal']);
    });
  });

  group('two fingers', () {
    test('second finger during an adjust keeps the value (no rollback), ends the adjust, and starts pinch / pan', () {
      down(1, 100, 100);
      move(1, 120, 100);
      move(1, 140, 100);
      expect(a.state, ArbiterState.adjustH);
      t += 16;
      a.pointerDown(2, 300, 100, t);
      expect(r.log.where((e) => e.startsWith('adjustEnd')), ['adjustEnd:true']);
      expect(r.has('scaleStart'), isTrue);
      expect(a.state, ArbiterState.multi);
      final before = r.count('adjustMove');
      move(2, 340, 100);
      move(1, 100, 100);
      expect(r.count('adjustMove'), before); // finger 1 no longer adjusts
      expect(r.log.any((e) => e.startsWith('scaleUpdate')), isTrue);
      up(2, 340, 100);
      expect(r.has('scaleEnd'), isTrue);
      // the remaining finger does nothing until everything is up
      move(1, 200, 200);
      expect(r.count('adjustBegin'), 1);
      up(1, 200, 200);
      expect(a.state, ArbiterState.idle);
    });

    test('pinch scale = distance ratio', () {
      down(1, 100, 100);
      t += 16;
      a.pointerDown(2, 200, 100, t);
      move(2, 300, 100);
      expect(r.log.last, 'scaleUpdate:2.00');
    });

    test('second finger while a long press is showing the original: compare ends, pinch starts', () {
      down(1, 50, 50);
      a.tick(400);
      expect(r.log, contains('longPress:true'));
      t = 420;
      a.pointerDown(2, 150, 50, t);
      expect(r.log, containsAllInOrder(['longPress:true', 'longPress:false', 'scaleStart']));
    });

    test('second finger voids a pending tap', () {
      down(1, 10, 10);
      up(1, 10, 10);
      a.pointerDown(1, 10, 10, 50);
      a.pointerDown(2, 100, 100, 60);
      a.tick(1000);
      expect(r.has('tap'), isFalse);
    });

    test('a third finger is ignored', () {
      down(1, 100, 100);
      a.pointerDown(2, 200, 100, 16);
      final n = r.log.length;
      a.pointerDown(3, 300, 100, 32);
      a.pointerMove(3, 310, 100, 48);
      expect(r.log.length, n);
    });
  });

  group('browse mode', () {
    setUp(() => r.mode = ArbiterMode.browse);

    test('fit zoom: horizontal swipe pages, with the first 10 dp included so the page follows the finger', () {
      down(1, 200, 300);
      move(1, 215, 302);
      expect(a.state, ArbiterState.paging);
      move(1, 260, 302);
      up(1, 260, 302);
      expect(r.log.where((e) => e.startsWith('page')).first, 'pageDrag');
      expect(r.log.last, 'pageEnd');
      expect(r.has('adjustBegin'), isFalse);
    });

    test('fit zoom: vertical drag does nothing', () {
      down(1, 200, 300);
      move(1, 202, 340);
      move(1, 203, 400);
      up(1, 203, 400);
      expect(a.state, ArbiterState.idle);
      expect(r.log, isEmpty);
    });

    test('zoomed: one finger pans in any direction', () {
      r.zoomed = true;
      down(1, 200, 300);
      move(1, 205, 312);
      move(1, 210, 330);
      up(1, 210, 330);
      expect(r.count('panBy'), 2);
      expect(r.log.last, 'panEnd');
      expect(r.has('pageDrag'), isFalse);
    });

    test('adjust mode: panning a zoomed photo needs two fingers (one finger adjusts)', () {
      r
        ..mode = ArbiterMode.adjust
        ..zoomed = true;
      down(1, 200, 300);
      move(1, 220, 300);
      expect(r.has('panBy'), isFalse);
      expect(r.has('adjustBegin'), isTrue);
    });

    test('touch exploration on: one finger never adjusts (behaves like browse)', () {
      r
        ..mode = ArbiterMode.adjust
        ..adjustEnabled = false;
      down(1, 200, 300);
      move(1, 230, 300);
      expect(r.has('adjustBegin'), isFalse);
    });
  });

  group('handedness samples', () {
    test('a stroke of >= 60 dp and every tap produce one sample with the touch ellipse', () {
      a.pointerDown(1, 300, 600, 0, orientation: -0.3, radiusMajor: 12, radiusMinor: 8);
      for (var i = 1; i <= 8; i++) {
        a.pointerMove(1, 300 - i * 10.0, 600 - i * 3.0 - (i * i * 0.2), i * 16);
      }
      a.pointerUp(1, 220, 560, 140);
      expect(r.strokes, hasLength(1));
      expect(r.strokes.first.isTap, isFalse);
      expect(r.strokes.first.orientation, -0.3);
      expect(r.strokes.first.downX, 300);
      a.pointerDown(1, 50, 50, 500);
      a.pointerUp(1, 50, 50, 520);
      expect(r.strokes, hasLength(2));
      expect(r.strokes.last.isTap, isTrue);
    });

    test('short strokes and vertical switches are not sampled', () {
      down(1, 100, 100);
      move(1, 130, 100);
      up(1, 130, 100);
      expect(r.strokes, isEmpty);
    });
  });

  test('reset ends a running gesture without leaving state behind', () {
    down(1, 0, 0);
    move(1, 30, 0);
    a.reset();
    expect(a.state, ArbiterState.idle);
    expect(r.log.last, 'adjustEnd:false');
  });
}
