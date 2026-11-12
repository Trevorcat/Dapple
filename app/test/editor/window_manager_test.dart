import 'dart:ui';

import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:flutter_test/flutter_test.dart';

LayoutClass phone() => LayoutClass.of(const Size(392.7, 872.7));
LayoutClass n5Inner() => LayoutClass.of(const Size(2480 / 3.0, 2248 / 3.0));
LayoutClass landscapePhone() => LayoutClass.of(const Size(844, 390));

WindowManager mk({Hand side = Hand.right, LayoutClass? lc}) {
  final m = WindowManager(side: side);
  m.setLayout(lc ?? phone());
  return m;
}

void main() {
  group('layout classes (by size and aspect, never by device)', () {
    test('phone portrait: compact, portrait, one window, no persistent histogram', () {
      final l = phone();
      expect(l.width, WidthClass.compact);
      expect(l.aspect, AspectClass.portrait);
      expect(l.maxWindows, 1);
      expect(l.histWindow, isFalse);
      expect(l.windowWidth, 196);
    });

    test('Find N5 inner (about 827 x 749 dp): near square, two windows and the histogram window', () {
      final l = n5Inner();
      expect(l.aspect, AspectClass.square);
      expect(l.width, WidthClass.medium);
      expect(l.maxWindows, 2);
      expect(l.histWindow, isTrue);
    });

    test('phone landscape (height < 480): side rail dock, a single window of 220', () {
      final l = landscapePhone();
      expect(l.railDock, isTrue);
      expect(l.maxWindows, 1);
      expect(l.windowWidth, 220);
    });

    test('large text widens windows by 32 and grows rows', () {
      final l = LayoutClass.of(const Size(400, 900), textScale: 1.4);
      expect(l.windowWidth, 196 + 32);
      expect(l.rowHeight, 56);
    });

    test('medium portrait needs a 55 % wide photo middle for a second window', () {
      final l = LayoutClass.of(const Size(626, 890));
      expect(l.maxWindows, 1);
      expect(LayoutClass.of(const Size(700, 900)).maxWindows, 1);
    });
  });

  group('opening, replacing, closing', () {
    test('compact: one window; another category replaces the content in place', () {
      final m = mk();
      m.toggle(EGroup.light);
      final anchor = m.windows.single.anchor;
      m.toggle(EGroup.color);
      expect(m.windows, hasLength(1));
      expect(m.windows.single.group, EGroup.color);
      expect(m.windows.single.anchor, anchor);
      m.toggle(EGroup.color); // tapping the shown category closes it
      expect(m.adjustMode, isFalse);
    });

    test('a replaced collapsed window opens expanded', () {
      final m = mk();
      m.toggle(EGroup.light);
      m.setCollapsed(EGroup.light, true);
      m.toggle(EGroup.crop);
      expect(m.windows.single.collapsed, isFalse);
    });

    test('wide: two windows, the third replaces the least recently used', () {
      final m = mk(lc: n5Inner());
      m.toggle(EGroup.light);
      m.toggle(EGroup.color);
      expect(m.windows, hasLength(2));
      m.focus(EGroup.light); // light is now the most recent
      m.toggle(EGroup.curve);
      expect(m.windows.map((w) => w.group).toSet(), {EGroup.light, EGroup.curve});
      expect(m.focused!.group, EGroup.curve);
    });

    test('closeLatest closes the most recently touched window (system back)', () {
      final m = mk(lc: n5Inner());
      m.toggle(EGroup.light);
      m.toggle(EGroup.color);
      expect(m.closeLatest(), isTrue);
      expect(m.windows.single.group, EGroup.light);
      expect(m.closeLatest(), isTrue);
      expect(m.closeLatest(), isFalse);
    });

    test('tap on empty photo: expanded windows collapse to pills, all pills expand again, never leaves adjust mode', () {
      final m = mk(lc: n5Inner());
      m.toggle(EGroup.light);
      m.toggle(EGroup.color);
      m.tapEmptyPhoto();
      expect(m.windows.every((w) => w.collapsed), isTrue);
      expect(m.adjustMode, isTrue);
      m.tapEmptyPhoto();
      expect(m.windows.every((w) => !w.collapsed), isTrue);
    });
  });

  group('handedness side', () {
    test('default anchors: first window on the dominant side, second on the other', () {
      final r = mk(lc: n5Inner());
      r.toggle(EGroup.light);
      r.toggle(EGroup.color);
      expect(r.windows[0].anchor.edge, DockEdge.right);
      expect(r.windows[1].anchor.edge, DockEdge.left);
      final l = mk(side: Hand.left, lc: n5Inner());
      l.toggle(EGroup.light);
      expect(l.windows[0].anchor.edge, DockEdge.left);
    });

    test('the side cannot change while an interaction is in progress', () {
      final m = mk();
      m.toggle(EGroup.light);
      m.setInteracting(true);
      expect(m.setSide(Hand.left), isFalse);
      expect(m.side, Hand.right);
      expect(m.windows.single.anchor.edge, DockEdge.right); // nothing moved
      m.setInteracting(false);
      expect(m.setSide(Hand.left), isTrue);
      expect(m.windows.single.anchor.edge, DockEdge.left);
    });

    test('windows the user placed stay put on an automatic switch; a manual choice clears the pins', () {
      final m = mk(lc: n5Inner());
      m.toggle(EGroup.light);
      m.toggle(EGroup.color);
      m.drop(EGroup.color, const Anchor(DockEdge.right, 0.3)); // user drags the 2nd window to the right
      m.setSide(Hand.left);
      expect(m.windowOf(EGroup.color)!.anchor, const Anchor(DockEdge.right, 0.3));
      expect(m.windowOf(EGroup.light)!.anchor.edge, DockEdge.left);
      m.setSide(Hand.right, manual: true);
      expect(m.windowOf(EGroup.color)!.pinned, isFalse);
      expect(m.windowOf(EGroup.color)!.anchor.edge, DockEdge.left);
      expect(m.windowOf(EGroup.light)!.anchor.edge, DockEdge.right);
    });

    test('histogram window sits at the top corner of the non-dominant side', () {
      expect(mk(lc: n5Inner()).histCorner, HistCorner.topLeft);
      expect(mk(side: Hand.left, lc: n5Inner()).histCorner, HistCorner.topRight);
    });
  });

  group('snapping', () {
    const area = WindowArea(bounds: Rect.fromLTWH(0, 100, 400, 600));
    const size = Size(196, 300);

    test('a drop near the right edge snaps right, near the left snaps left, near the bottom snaps bottom; never the top', () {
      expect(anchorForDrop(const Rect.fromLTWH(180, 200, 196, 300), Offset.zero, size, area).edge, DockEdge.right);
      expect(anchorForDrop(const Rect.fromLTWH(20, 200, 196, 300), Offset.zero, size, area).edge, DockEdge.left);
      expect(anchorForDrop(const Rect.fromLTWH(100, 395, 196, 300), Offset.zero, const Size(196, 300), area).edge, DockEdge.bottom);
      final up = anchorForDrop(const Rect.fromLTWH(10, 105, 196, 300), const Offset(0, -2000), size, area);
      expect(up.edge, DockEdge.left);
      expect(up.t, 0);
    });

    test('the edge a window came from is sticky: a small drag never flips it to the bottom edge', () {
      const resting = Rect.fromLTWH(192, 400, 196, 300); // right side, bottom corner of the area
      final small = anchorForDrop(resting.shift(const Offset(-20, 0)), Offset.zero, size, area, current: DockEdge.right);
      expect(small.edge, DockEdge.right);
      final far = anchorForDrop(resting.shift(const Offset(-150, 0)), Offset.zero, size, area, current: DockEdge.right);
      expect(far.edge, DockEdge.bottom);
    });

    test('velocity projection (x 0.099 s) decides: a flick to the right from the middle snaps right', () {
      final a = anchorForDrop(const Rect.fromLTWH(100, 200, 196, 300), const Offset(2500, 0), size, area);
      expect(a.edge, DockEdge.right);
    });

    test('side snaps are free along the edge, clamped between the top bar and the dock', () {
      final a = anchorForDrop(const Rect.fromLTWH(200, 50, 196, 300), Offset.zero, size, area);
      expect(a.t, 0);
      final r = windowRect(a, size, area);
      expect(r.top, 100);
      final b = windowRect(const Anchor(DockEdge.right, 1), size, area);
      expect(b.bottom, 700);
      expect(b.right, 400 - 12);
    });

    test('margins respect the system gesture insets', () {
      const wide = WindowArea(bounds: Rect.fromLTWH(0, 0, 400, 800), marginLeft: 30, marginRight: 24);
      expect(windowRect(const Anchor(DockEdge.left), size, wide).left, 30);
      expect(windowRect(const Anchor(DockEdge.right), size, wide).right, 376);
    });

    test('a window taller than the area is pinned to the top instead of overflowing', () {
      final r = windowRect(const Anchor(DockEdge.left, 1), const Size(196, 900), area);
      expect(r.top, 100);
    });

    test('histogram window snaps to the nearest corner', () {
      const a = WindowArea(bounds: Rect.fromLTWH(0, 80, 800, 700));
      expect(histCornerForDrop(const Rect.fromLTWH(600, 600, 168, 88), a), HistCorner.bottomRight);
      expect(histCornerForDrop(const Rect.fromLTWH(20, 90, 168, 88), a), HistCorner.topLeft);
      final r = histogramRect(HistCorner.topRight, a);
      expect(r.size, const Size(168, 88));
      expect(r.right, 788);
      expect(histogramRect(HistCorner.topLeft, a, small: true).size, const Size(120, 64));
    });
  });

  group('window classes (fold / rotate)', () {
    test('placement is remembered per class and restored when the class returns', () {
      final m = mk(lc: n5Inner());
      m.toggle(EGroup.light);
      m.drop(EGroup.light, const Anchor(DockEdge.left, 0.4));
      m.setLayout(phone());
      expect(m.windows.single.anchor.edge, DockEdge.right); // default of the new class
      expect(m.windows.single.group, EGroup.light); // content kept
      m.setLayout(n5Inner());
      expect(m.windows.single.anchor, const Anchor(DockEdge.left, 0.4));
    });

    test('folding trims to the window limit; unfolding brings the second window back', () {
      final m = mk(lc: n5Inner());
      m.toggle(EGroup.light);
      m.toggle(EGroup.color);
      m.focus(EGroup.color);
      m.setLayout(phone());
      expect(m.windows.single.group, EGroup.color); // the one in use stays
      m.setLayout(n5Inner());
      expect(m.windows.map((w) => w.group).toSet(), {EGroup.light, EGroup.color});
    });

    test('the same class again changes nothing', () {
      final m = mk();
      expect(m.setLayout(phone()), isFalse);
    });
  });

  test('memory survives a json round trip', () {
    final m = mk(lc: n5Inner());
    m.toggle(EGroup.light);
    m.drop(EGroup.light, const Anchor(DockEdge.bottom, 0.25));
    m.dropHistogram(HistCorner.bottomLeft);
    final json = m.toJsonString();
    final m2 = WindowManager()..restore(json);
    m2.setLayout(n5Inner());
    m2.toggle(EGroup.light);
    expect(m2.windows.single.anchor, const Anchor(DockEdge.bottom, 0.25));
    expect(m2.histCorner, HistCorner.bottomLeft);
    expect(WindowManager()..restore('not json'), isA<WindowManager>());
  });
}
