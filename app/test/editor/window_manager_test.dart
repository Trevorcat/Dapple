// The panel state of the editor (which category is open, folded or not, the hand side). The geometry has its own
// tests in panel_layout_test.dart.
import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a dock tap opens a category, replaces it with another, and closes it when tapped again', () {
    final wm = WindowManager();
    var n = 0;
    wm.addListener(() => n++);
    expect(wm.adjustMode, isFalse);
    wm.toggle(EGroup.light);
    expect(wm.focused?.group, EGroup.light);
    expect(wm.adjustMode, isTrue);
    wm.toggle(EGroup.color);
    expect(wm.windows.map((w) => w.group), [EGroup.color], reason: 'one panel: the new category replaces the old');
    wm.toggle(EGroup.color);
    expect(wm.focused, isNull);
    expect(n, 3);
  });

  test('fold, unfold, tap on the photo and back', () {
    final wm = WindowManager()..toggle(EGroup.light);
    wm.setCollapsed(EGroup.light, true);
    expect(wm.focused!.collapsed, isTrue);
    wm.tapEmptyPhoto();
    expect(wm.focused!.collapsed, isFalse, reason: 'a tap on the photo opens a folded panel');
    wm.tapEmptyPhoto();
    expect(wm.focused!.collapsed, isTrue);
    wm.setCollapsed(EGroup.color, false); // not open: nothing happens
    expect(wm.windowOf(EGroup.color), isNull);
    expect(wm.closeLatest(), isTrue);
    expect(wm.closeLatest(), isFalse);
  });

  test('opening another category starts unfolded', () {
    final wm = WindowManager()..toggle(EGroup.light);
    wm.setCollapsed(EGroup.light, true);
    wm.toggle(EGroup.style);
    expect(wm.focused!.collapsed, isFalse);
  });

  test('the hand side changes unless a finger is down; a manual choice always applies', () {
    final wm = WindowManager(side: Hand.right);
    wm.setInteracting(true);
    expect(wm.setSide(Hand.left), isFalse);
    expect(wm.side, Hand.right);
    wm.setInteracting(false);
    expect(wm.setSide(Hand.left), isTrue);
    expect(wm.side, Hand.left);
    expect(otherHand(Hand.left), Hand.right);
  });
}
