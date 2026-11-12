// Docked control panel of the phone-portrait editor (docs/design/editor-panel.md): chips, the one big
// ruler, 2D pads, collapse, back order, photo never covered, and that large layouts keep their windows.
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/dock_panel.dart' show kPanelCollapsedH, kPanelHandleH, kPanelStripH;
import 'package:dapple/screens/editor/editor_overlays.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/photo_art.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, dump, n5Dpr, n5Phys, open, phoneDpr, phonePhys, setView, settle, tapKey, zoomOf;
import 'support/harness.dart';

const ownerPhys = Size(1140, 2616); // the owner's foldable outer screen
const ownerDpr = 2.75;

void main() {
  largeTextTests();
  group('phone portrait', () {
    testWidgets('browse mode: photo fills the screen, top bar and dock float, no panel, no histogram window', (t) async {
      await open(t);
      expect(find.byKey(const ValueKey('toolbar-back')), findsOneWidget);
      expect(find.byKey(const ValueKey('toolbar-undo')), findsOneWidget);
      expect(find.byKey(const ValueKey('toolbar-redo')), findsOneWidget);
      expect(find.byKey(const ValueKey('toolbar-save')), findsOneWidget);
      for (final l in ['光线', '颜色', '风格', '裁切', '效果', '更多']) {
        expect(find.text(l), findsWidgets, reason: l);
      }
      for (final l in ['曲线', '细节', '分级']) {
        expect(find.text(l), findsNothing, reason: '$l lives in 更多');
      }
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
      expect(find.byKey(const ValueKey('histogram-window')), findsNothing);
      expect(find.byType(Slider), findsNothing);
      await dump(t, 'phone_browse');
    });

    testWidgets('layout switch: phone portrait docks the panel, no floating window exists', (t) async {
      await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 10);
      expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
      expect(find.byKey(const ValueKey('panel-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
      expect(find.byKey(const ValueKey('pill-light')), findsNothing);
      for (final k in ['exposure', 'contrast', 'highlights', 'shadows', 'whites', 'blacks']) {
        expect(find.byKey(ValueKey('chip-light-$k')), findsOneWidget, reason: k);
      }
      expect(find.byKey(const ValueKey('panel-ruler')), findsOneWidget, reason: 'one big ruler for the selected parameter');
      expect(find.byType(Slider), findsNothing);
      await dump(t, 'phone_panel_light');
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 10);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
    });

    testWidgets('chips select the parameter; the ruler then edits that one; chips are at least 44 dp tall', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      expect(t.getRect(find.byKey(const ValueKey('chip-light-contrast'))).height, greaterThanOrEqualTo(44));
      await chipTap(t, 'chip-light-contrast');
      final e0 = app.editOf(id).value('exposure'), c0 = app.editOf(id).value('contrast');
      final r = t.getRect(find.byKey(const ValueKey('panel-ruler')));
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(100, 0));
      await f.up();
      await settle(t, 3);
      expect(app.editOf(id).value('exposure'), e0);
      expect(app.editOf(id).value('contrast'), greaterThan(c0 + 8));
    });

    testWidgets('chips are sized to their content: four to five are fully visible across a 414 dp screen', (t) async {
      await open(t, title: '街角咖啡', phys: ownerPhys, dpr: ownerDpr);
      await dockTap(t, 'dock-${EGroup.light}');
      var visible = 0;
      for (final k in ['exposure', 'contrast', 'highlights', 'shadows', 'whites', 'blacks']) {
        final r = t.getRect(find.byKey(ValueKey('chip-light-$k')));
        expect(r.height, greaterThanOrEqualTo(44), reason: k);
        expect(r.width, inInclusiveRange(60, 140), reason: k);
        if (r.left >= 0 && r.right <= 414.5 - 48) visible++;
      }
      expect(visible, greaterThanOrEqualTo(4));
    });

    testWidgets('the ruler spans the full panel width and a drag on it equals a drag on the photo; one undo step', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 10);
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      final ruler = t.getRect(find.byKey(const ValueKey('panel-ruler')));
      expect(ruler.width, closeTo(panel.width - 2, 2), reason: 'full panel width');
      expect(ruler.height, greaterThanOrEqualTo(56));
      final e0 = app.editOf(id).value('exposure');
      final f = await Finger.down(t, ruler.center);
      await f.drag(const Offset(150, 4));
      await t.pump(const Duration(milliseconds: 100));
      expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget, reason: 'the panel stays during the drag');
      expect(find.byKey(const ValueKey('readout')), findsNothing, reason: 'nothing over the photo');
      await f.up();
      await settle(t);
      final byRuler = app.editOf(id).value('exposure') - e0;
      expect(byRuler, greaterThan(15));
      app.undo(id);
      expect(app.canUndo(id), isFalse, reason: 'one gesture = one undo step');
      expect(app.editOf(id).value('exposure'), e0);
      await settle(t, 3);
      final g = await Finger.down(t, const Offset(60, 300));
      await g.drag(const Offset(150, 4));
      await g.up();
      await settle(t);
      final byPhoto = app.editOf(id).value('exposure') - e0;
      expect(byPhoto, greaterThan(15));
      expect((byPhoto - byRuler).abs(), lessThan(byRuler * 0.35), reason: 'ruler $byRuler vs photo $byPhoto');
    });

    testWidgets('a drag on the photo shows live in the big value (sync); the selected ruler chip shows only its name; dragging far above the ruler is a finer band', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 10);
      expect(find.descendant(of: find.byKey(const ValueKey('chip-light-exposure')), matching: find.byType(Text)), findsOneWidget, reason: 'name only: the value is the big number');
      String big() => t.widget<Text>(find.descendant(of: find.byKey(const ValueKey('value-exposure')), matching: find.byType(Text))).data!;
      final before = big();
      final f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(120, 2));
      await t.pump(const Duration(milliseconds: 50));
      expect(big(), isNot(before), reason: 'live value above the ruler while the photo is dragged');
      await f.up();
      await settle(t);
      final r = t.getRect(find.byKey(const ValueKey('panel-ruler')));
      app.applyEdit(id, (e) => e.withValue('exposure', 0));
      await settle(t, 3);
      final h = await Finger.down(t, r.center);
      await h.drag(const Offset(10, 0), step: 5);
      await h.drag(const Offset(0, -220), step: 8);
      final b = app.editOf(id).value('exposure');
      await h.drag(const Offset(100, 0), step: 5);
      await h.up();
      expect(app.editOf(id).value('exposure') - b, lessThan(7), reason: 'ultra band: about 1/16 of a normal 100 dp stroke');
    });

    testWidgets('double tap on the ruler resets the parameter; chips show a changed mark; category reset in the strip', (t) async {
      final (app, id) = await open(t);
      app.applyEdit(id, (e) => e.withValue('exposure', 30).withValue('contrast', -20));
      await dockTap(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('mark-exposure')), findsOneWidget);
      expect(find.byKey(const ValueKey('mark-contrast')), findsOneWidget);
      expect(find.byKey(const ValueKey('mark-shadows')), findsNothing);
      final c = t.getCenter(find.byKey(const ValueKey('panel-ruler')));
      await t.tapAt(c);
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(c);
      await settle(t);
      expect(app.editOf(id).value('exposure'), 0);
      expect(app.editOf(id).value('contrast'), -20);
      await tapKey(t, 'reset-light');
      expect(app.editOf(id).value('contrast'), 0);
      expect(find.text('已复位光线'), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('toast-action')));
      await settle(t);
      expect(app.editOf(id).value('contrast'), -20, reason: 'the category reset is one undo step');
    });

    testWidgets('the small reset affordance and a double tap on the selected chip reset the parameter', (t) async {
      final (app, id) = await open(t);
      app.applyEdit(id, (e) => e.withValue('exposure', 30));
      await dockTap(t, 'dock-${EGroup.light}');
      expect(t.getSize(find.byKey(const ValueKey('reset-row'))).height, greaterThanOrEqualTo(44));
      await tapKey(t, 'reset-row');
      expect(app.editOf(id).value('exposure'), 0);
      app.applyEdit(id, (e) => e.withValue('exposure', 30));
      await settle(t, 3);
      final chip = find.byKey(const ValueKey('chip-light-exposure'));
      await t.tap(chip);
      await t.pump(const Duration(milliseconds: 60));
      await t.tap(chip);
      await settle(t, 3);
      expect(app.editOf(id).value('exposure'), 0);
    });

    testWidgets('tapping the value opens numeric entry; EV is typed in EV; out of range is clamped', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      await tapKey(t, 'value-exposure');
      expect(find.byType(TextField), findsOneWidget);
      await t.enterText(find.byType(TextField), '0.5');
      await t.testTextInput.receiveAction(TextInputAction.done);
      await settle(t);
      expect(app.editOf(id).value('exposure'), 10);
      await tapKey(t, 'value-exposure');
      await t.enterText(find.byType(TextField), '9');
      await t.testTextInput.receiveAction(TextInputAction.done);
      await settle(t);
      expect(app.editOf(id).value('exposure'), 100);
      expect(find.text('已调整到范围内'), findsOneWidget);
    });

    testWidgets('the photo is never overlapped by the panel: portrait and landscape photos, every category, folded too', (t) async {
      for (final (title, phys, dpr) in [('街角咖啡', phonePhys, phoneDpr), ('外滩夕阳', phonePhys, phoneDpr), ('街角咖啡', ownerPhys, ownerDpr), ('外滩夕阳', ownerPhys, ownerDpr)]) {
        await open(t, title: title, phys: phys, dpr: dpr);
        final screen = phys / dpr;
        for (final g in [EGroup.light, EGroup.color, EGroup.style, EGroup.crop, EGroup.effects]) {
          await dockTap(t, 'dock-$g');
          await settle(t, 30);
          final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
          final photo = t.getRect(find.byType(PhotoArt));
          expect(photo.bottom, lessThanOrEqualTo(panel.top + 0.5), reason: '$title $g: photo $photo panel $panel');
          expect(photo.top, greaterThan(40), reason: 'below the status bar');
          expect(photo.height, greaterThan(140), reason: '$title $g: the photo keeps a usable size');
          expect(panel.height, greaterThanOrEqualTo(kPanelHandleH + kPanelStripH), reason: '$title $g');
          expect(panel.left, greaterThanOrEqualTo(0));
          expect(panel.right, lessThanOrEqualTo(screen.width));
          final dock = t.getRect(find.byType(CategoryDock));
          expect(panel.bottom, lessThanOrEqualTo(dock.top + 0.5), reason: '$title $g: panel $panel dock $dock');
          await tapKey(t, 'panel-handle');
          await settle(t, 30);
          final p2 = t.getRect(find.byKey(const ValueKey('docked-panel')));
          final ph2 = t.getRect(find.byType(PhotoArt));
          expect(p2.height, lessThan(panel.height + 0.5));
          expect(ph2.bottom, lessThanOrEqualTo(p2.top + 0.5), reason: '$title $g folded');
          expect(ph2.height, greaterThanOrEqualTo(photo.height - 0.5), reason: 'folding never shrinks the photo');
          await tapKey(t, 'panel-handle');
          await settle(t, 30);
          expect(t.getRect(find.byKey(const ValueKey('docked-panel'))).height, closeTo(panel.height, 1));
          await dockTap(t, 'dock-$g');
          await settle(t, 30);
        }
        await t.pumpWidget(const SizedBox());
      }
    });

    testWidgets('a landscape photo leaves a tall panel: wide histogram, a much larger pad', (t) async {
      await open(t, title: '外滩夕阳', phys: ownerPhys, dpr: ownerDpr);
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 30);
      final h = t.getRect(find.byKey(const ValueKey('docked-panel'))).height;
      expect(h, inInclusiveRange(300, 360), reason: 'hugs strip + ruler + a histogram of at most 120 dp: no blank band');
      expect(t.getRect(find.byKey(const ValueKey('panel-histogram'))).height, lessThanOrEqualTo(120.5));
      await dump(t, 'phone_panel_landscape_photo');
      await dockTap(t, 'dock-${EGroup.light}');
      await dockTap(t, 'dock-${EGroup.style}');
      await chipTap(t, 'chip-style-palette');
      await settle(t, 30);
      expect(t.getRect(find.byKey(const ValueKey('pad-palette'))).height, greaterThan(250));
      await dump(t, 'phone_panel_landscape_pad');
    });

    testWidgets('pad sizing on 1140x2616 @ 2.75: full panel width, >= 40 % of the screen width tall, no scroll view around it', (t) async {
      await open(t, title: '街角咖啡', phys: ownerPhys, dpr: ownerDpr);
      final screenW = 1140 / 2.75;
      await dockTap(t, 'dock-${EGroup.style}');
      await chipTap(t, 'chip-style-palette');
      await settle(t, 30);
      final pad = t.getRect(find.byKey(const ValueKey('pad-palette')));
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(pad.width, greaterThanOrEqualTo(panel.width - 28), reason: 'full panel width (12 dp margins)');
      expect(pad.height, greaterThanOrEqualTo(screenW * 0.4 - 0.5), reason: 'pad $pad');
      expect(pad.height * 2.75, greaterThan(400), reason: 'far bigger than the old 72 dp map');
      expect(find.ancestor(of: find.byKey(const ValueKey('pad-palette')), matching: find.byType(Scrollable)), findsNothing, reason: 'no gesture conflict');
      expect(find.byKey(const ValueKey('panel-ruler')), findsNothing);
      await dump(t, 'phone_panel_pad');
    });

    testWidgets('colour pad: a drag changes the palette, one undo step, the chip shows the readout, the photo does not move', (t) async {
      final (app, id) = await open(t, title: '街角咖啡', phys: ownerPhys, dpr: ownerDpr);
      await dockTap(t, 'dock-${EGroup.style}');
      await chipTap(t, 'chip-style-palette');
      await settle(t, 30);
      expect(app.editOf(id).palette, isNull);
      final pad = t.getRect(find.byKey(const ValueKey('pad-palette')));
      final photoBefore = t.getRect(find.byType(PhotoArt));
      final f = await Finger.down(t, pad.center);
      await f.drag(const Offset(60, -80), step: 6);
      await t.pump(const Duration(milliseconds: 100));
      expect(find.byKey(const ValueKey('readout')), findsNothing);
      expect(find.descendant(of: find.byKey(const ValueKey('chip-style-palette')), matching: find.textContaining('偏')), findsOneWidget, reason: 'readout in the chip');
      await f.drag(const Offset(30, -30));
      await f.up();
      await settle(t);
      final p = app.editOf(id).palette;
      expect(p, isNotNull);
      expect(p!.dx, greaterThan(0.5));
      expect(p.dy, lessThan(0.5), reason: 'up = warm');
      expect(t.getRect(find.byType(PhotoArt)), photoBefore);
      app.undo(id);
      expect(app.canUndo(id), isFalse);
    });

    testWidgets('style without a film pack: chips for the look rows, the look strip as lower tier', (t) async {
      await open(t);
      await dockTap(t, 'dock-${EGroup.style}');
      for (final k in ['look', 'lookStrength', 'palette', 'toneTarget', 'toneStrength']) {
        expect(find.byKey(ValueKey('chip-style-$k')), findsOneWidget, reason: k);
      }
      expect(find.byKey(const ValueKey('chip-style-film')), findsNothing);
      await chipTap(t, 'chip-style-look');
      expect(find.byKey(const ValueKey('extra-look')), findsOneWidget);
      await dump(t, 'phone_style');
    });

    testWidgets('colour range, curve, grading, crop and detail render their lower tier without overflow', (t) async {
      await open(t);
      await dockTap(t, 'dock-${EGroup.color}');
      await chipTap(t, 'chip-color-band');
      await tapKey(t, 'band-5');
      expect(find.text('蓝色相'), findsOneWidget);
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-curve');
      await chipTap(t, 'chip-curve-cv_rgb_L');
      expect(find.byKey(const ValueKey('pad-cv_rgb_L')), findsOneWidget);
      await dump(t, 'phone_panel_curve');
      await chipTap(t, 'chip-curve-channel');
      expect(find.byKey(const ValueKey('channel-2')), findsOneWidget);
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-grade');
      await chipTap(t, 'chip-grade-wheel');
      expect(find.byKey(const ValueKey('pad-wheel')), findsOneWidget);
      await dump(t, 'phone_panel_grade');
      await dockTap(t, 'dock-${EGroup.crop}');
      await chipTap(t, 'chip-crop-ratio');
      expect(find.byKey(const ValueKey('crop-4')), findsOneWidget);
      await dump(t, 'phone_panel_crop');
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-detail');
      await settle(t, 10);
      expect(find.byKey(const ValueKey('chip-detail-sharpen')), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    testWidgets('curve pad: a vertical drag changes the selected point; a tap near the left edge selects the shadow point', (t) async {
      final (app, id) = await open(t, title: '街角咖啡', phys: ownerPhys, dpr: ownerDpr);
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-curve');
      await chipTap(t, 'chip-curve-cv_rgb_H');
      await settle(t, 30);
      final pad = t.getRect(find.byKey(const ValueKey('pad-cv_rgb_H')));
      final f = await Finger.down(t, pad.center);
      await f.drag(const Offset(0, -60), step: 6);
      await f.up();
      await settle(t, 3);
      expect(app.editOf(id).value('cv_rgb_H'), greaterThan(10), reason: 'up raises the point');
      await t.tapAt(Offset(pad.left + pad.width * 0.12, pad.center.dy));
      await settle(t, 5);
      expect(find.byKey(const ValueKey('pad-cv_rgb_S')), findsOneWidget);
    });

    testWidgets('collapse: the handle folds the panel to the chip strip, the photo takes the space; a chip tap opens it again', (t) async {
      await open(t, title: '街角咖啡');
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 30);
      final full = t.getRect(find.byKey(const ValueKey('docked-panel')));
      final photoFull = t.getRect(find.byType(PhotoArt));
      await tapKey(t, 'panel-handle');
      await settle(t, 30);
      final small = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(small.height, closeTo(kPanelCollapsedH, 1));
      expect(find.byKey(const ValueKey('panel-ruler')), findsNothing);
      expect(find.byKey(const ValueKey('chip-light-exposure')), findsOneWidget);
      expect(t.getRect(find.byType(PhotoArt)).height, greaterThan(photoFull.height - 0.5));
      expect(small.height, lessThan(full.height));
      await dump(t, 'phone_panel_collapsed');
      await chipTap(t, 'chip-light-contrast');
      await settle(t, 30);
      expect(find.byKey(const ValueKey('panel-ruler')), findsOneWidget);
      expect(t.getRect(find.byKey(const ValueKey('docked-panel'))).height, closeTo(full.height, 1));
    });

    testWidgets('system back dismisses in order: numeric entry, panel fold, panel close, then the page', (t) async {
      await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      await tapKey(t, 'value-exposure');
      expect(find.byType(TextField), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t);
      expect(find.byType(TextField), findsNothing);
      expect(find.byKey(const ValueKey('panel-ruler')), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t, 20);
      expect(find.byKey(const ValueKey('panel-ruler')), findsNothing, reason: 'folded to the chip strip');
      expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t, 20);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing, reason: 'closed');
    });

    testWidgets('long press shows the original and releases; moving afterwards does not edit', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      final f = await Finger.down(t, const Offset(60, 300));
      await t.pump(const Duration(milliseconds: 400));
      await t.pump(const Duration(milliseconds: 200));
      expect(find.byKey(const ValueKey('original-pill')), findsOneWidget);
      final e0 = app.editOf(id).value('exposure');
      await f.drag(const Offset(100, 0));
      expect(app.editOf(id).value('exposure'), e0);
      await f.up();
      await settle(t);
      expect(find.byKey(const ValueKey('original-pill')), findsNothing);
    });

    testWidgets('the vertical lock on the photo switches the selected chip; the next horizontal drag changes it', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      final e0 = app.editOf(id).value('exposure'), c0 = app.editOf(id).value('contrast');
      final f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(1, 60), step: 5);
      await f.up();
      await settle(t, 6);
      expect(app.editOf(id).value('exposure'), e0, reason: 'vertical never changes a value');
      final g = await Finger.down(t, const Offset(60, 300), startMs: 2000);
      await g.drag(const Offset(120, 2));
      await g.up();
      expect(app.editOf(id).value('exposure'), e0);
      expect(app.editOf(id).value('contrast'), greaterThan(c0 + 5));
    });

    testWidgets('a pause longer than 800 ms inside one ruler gesture is still one undo step', (t) async {
      final (app, id) = await open(t);
      await dockTap(t, 'dock-${EGroup.light}');
      final e0 = app.editOf(id).value('exposure');
      final r = t.getRect(find.byKey(const ValueKey('panel-ruler')));
      final f = await Finger.down(t, r.center - const Offset(100, 0));
      await f.drag(const Offset(60, 0));
      await t.pump(const Duration(milliseconds: 1200));
      await f.drag(const Offset(60, 0));
      await f.up();
      expect(app.editOf(id).value('exposure'), isNot(e0));
      app.undo(id);
      expect(app.canUndo(id), isFalse);
      expect(app.editOf(id).value('exposure'), e0);
    });

    testWidgets('tapping empty photo folds the panel; again opens it', (t) async {
      await open(t, title: '街角咖啡');
      await dockTap(t, 'dock-${EGroup.light}');
      await settle(t, 20);
      await t.tapAt(const Offset(60, 200));
      await settle(t, 30);
      expect(find.byKey(const ValueKey('panel-ruler')), findsNothing);
      await t.tapAt(const Offset(60, 200));
      await settle(t, 30);
      expect(find.byKey(const ValueKey('panel-ruler')), findsOneWidget);
    });

    testWidgets('browse mode: tap toggles immersive, double tap zooms, two fingers zoom', (t) async {
      await open(t);
      final z = zoomOf(t);
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      expect(t.widget<AnimatedOpacity>(find.ancestor(of: find.byKey(const ValueKey('toolbar-back')), matching: find.byType(AnimatedOpacity)).first).opacity, 0);
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(const Offset(200, 400));
      await settle(t, 8);
      expect(z.scale, closeTo(2, 0.01));
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(const Offset(200, 400));
      await settle(t, 8);
      expect(z.scale, closeTo(1, 0.01));
      final a = await Finger.down(t, const Offset(150, 400));
      final b = await Finger.down(t, const Offset(250, 400), startMs: 0);
      await a.by(const Offset(-40, 0));
      await b.by(const Offset(40, 0));
      await a.up();
      await b.up();
      await settle(t, 8);
      expect(z.scale, greaterThan(1.5));
    });

    testWidgets('browse mode: a one-finger swipe at fit zoom changes the photo and shows n / N', (t) async {
      await open(t);
      final f = await Finger.down(t, const Offset(300, 400));
      await f.drag(const Offset(-200, 2), step: 10);
      await f.up();
      for (var i = 0; i < 4; i++) {
        await t.pump(const Duration(milliseconds: 50));
      }
      expect(find.textContaining(' / '), findsOneWidget);
      await settle(t, 10);
    });

    testWidgets('dock: six fixed items, no horizontal scrolling, every item fully on screen and >= 48 dp', (t) async {
      await open(t);
      final sv = t.widget<SingleChildScrollView>(find.descendant(of: find.byType(CategoryDock), matching: find.byType(SingleChildScrollView)));
      expect(sv.physics, isA<NeverScrollableScrollPhysics>());
      final screen = t.view.physicalSize / t.view.devicePixelRatio;
      for (final k in [...phoneDockOrder.map((g) => 'dock-$g'), 'dock-more']) {
        final r = t.getRect(find.byKey(ValueKey(k)));
        expect(r.left, greaterThanOrEqualTo(0), reason: k);
        expect(r.right, lessThanOrEqualTo(screen.width), reason: k);
        expect(r.width, greaterThanOrEqualTo(48), reason: k);
        expect(r.height, greaterThanOrEqualTo(48), reason: k);
      }
    });

    testWidgets('moving between parameters of one category never lets the panel cover the photo; pickers do not leave a void', (t) async {
      await open(t, title: '外滩夕阳', phys: ownerPhys, dpr: ownerDpr);
      await dockTap(t, 'dock-${EGroup.color}');
      for (final k in ['chip-color-band', 'chip-color-temp', 'chip-color-saturation']) {
        await chipTap(t, k);
        await settle(t, 20);
        final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
        expect(t.getRect(find.byType(PhotoArt)).bottom, lessThanOrEqualTo(panel.top + 0.5), reason: k);
        expect(panel.height, lessThan(260), reason: '$k: a plain ruler / picker stays near its natural height');
      }
    });

    testWidgets('undo / redo buttons work; the more menu holds the old actions', (t) async {
      final (app, id) = await open(t);
      final e0 = app.editOf(id).value('exposure');
      app.applyEdit(id, (e) => e.withValue('exposure', 40));
      await settle(t, 2);
      await tapKey(t, 'toolbar-undo');
      expect(app.editOf(id).value('exposure'), e0);
      await tapKey(t, 'toolbar-redo');
      expect(app.editOf(id).value('exposure'), 40);
      await tapKey(t, 'dock-more');
      for (final k in ['menu-publish', 'menu-copy', 'menu-paste', 'menu-sync', 'menu-restore']) {
        expect(find.byKey(ValueKey(k)), findsOneWidget, reason: k);
      }
      await dump(t, 'phone_more_menu');
      await tapKey(t, 'menu-copy');
      expect(app.hasClipboard, isTrue);
    });

    testWidgets('dark mode', (t) async {
      await open(t, dark: true, phys: ownerPhys, dpr: ownerDpr);
      await dockTap(t, 'dock-${EGroup.light}');
      await dump(t, 'phone_dark_adjust');
      await dockTap(t, 'dock-${EGroup.style}');
      await chipTap(t, 'chip-style-palette');
      await dump(t, 'phone_dark_pad');
    });

    testWidgets('modified categories get a dot in the dock', (t) async {
      final (app, id) = await open(t);
      app.applyEdit(id, (_) => EditState.empty);
      await settle(t, 2);
      expect(find.byKey(const ValueKey('dot-light')), findsNothing);
      app.applyEdit(id, (e) => e.withValue('exposure', 10));
      await settle(t, 2);
      expect(find.byKey(const ValueKey('dot-light')), findsOneWidget);
    });

    testWidgets('hand side: the category reset sits on the far side of the thumb, 44 dp wide', (t) async {
      for (final (hand, farLeft) in [(1, false), (2, true)]) {
        setView(t, phonePhys, phoneDpr);
        await loadTestFonts();
        final app = AppState.demo();
        app.settings.handedness = hand;
        final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
        app.applyEdit(id, (e) => e.withValue('exposure', 30));
        await t.pumpWidget(RepaintBoundary(
            key: shotKey,
            child: MaterialApp(debugShowCheckedModeBanner: false, theme: buildTheme(Brightness.light), home: EditorPage(app: app, photoId: id, resamplePointers: false))));
        await settle(t);
        await dockTap(t, 'dock-${EGroup.light}');
        await settle(t, 20);
        final r = t.getRect(find.byKey(const ValueKey('reset-light')));
        expect(r.center.dx < phonePhys.width / phoneDpr / 2, farLeft, reason: 'hand $hand');
        expect(r.width, greaterThanOrEqualTo(44));
        await t.pumpWidget(const SizedBox());
      }
    });
  });

  group('large screens keep the floating windows', () {
    testWidgets('phone landscape: side rail dock, one floating window, no docked panel', (t) async {
      await open(t, phys: const Size(2400, 1080), dpr: 2.75);
      await dockTap(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
      await dump(t, 'phone_landscape');
    });

    testWidgets('inner screen / tablet width: floating windows, no docked panel', (t) async {
      await open(t, phys: n5Phys, dpr: n5Dpr);
      await dockTap(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
    });

    testWidgets('folding to the phone docks the panel, unfolding brings the window back', (t) async {
      await open(t, phys: n5Phys, dpr: n5Dpr);
      await dockTap(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      setView(t, phonePhys, phoneDpr);
      await settle(t, 20);
      expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
      setView(t, n5Phys, n5Dpr);
      await settle(t, 20);
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
    });

    testWidgets('a pad inside a floating window claims its drags: it edits, the window neither scrolls nor folds, and it is big enough', (t) async {
      final (app, id) = await open(t, phys: n5Phys, dpr: n5Dpr);
      await dockTap(t, 'dock-${EGroup.style}');
      await tapKey(t, 'row-style-palette');
      await settle(t, 20);
      final pad = find.byKey(const ValueKey('pad-palette'));
      await t.ensureVisible(pad);
      await settle(t, 5);
      final r = t.getRect(pad);
      expect(r.width, greaterThanOrEqualTo(150));
      expect(r.height, greaterThanOrEqualTo(120));
      final sc = find.descendant(of: find.byKey(const ValueKey('window-scroll')), matching: find.byType(Scrollable));
      final off0 = t.state<ScrollableState>(sc.first).position.pixels;
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(0, -50), step: 5); // vertical: would scroll the window if the pad did not claim it
      await t.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('window-style')).hitTestable(), findsOneWidget, reason: 'the window stays under the finger');
      await f.up();
      await settle(t, 5);
      expect(t.state<ScrollableState>(sc.first).position.pixels, closeTo(off0, 0.5));
      expect(app.editOf(id).palette, isNotNull);
    });
  });
}

/// Dock tap, then let the panel finish growing (its spring runs about 400 ms).
Future<void> dockTap(WidgetTester t, String k) async {
  await tapKey(t, k);
  await settle(t, 8);
}

/// Scrolls the chip strip to [k] and taps it, then lets the panel settle.
Future<void> chipTap(WidgetTester t, String k) async {
  final f = find.byKey(ValueKey(k));
  await t.ensureVisible(f);
  await settle(t, 2);
  await t.tap(f);
  await settle(t, 8);
}

void largeTextTests() {
  testWidgets('large text (1.6x): every tier lays out without overflow and the photo is not covered', (t) async {
    t.platformDispatcher.textScaleFactorTestValue = 1.6;
    addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
    await open(t, title: '街角咖啡', phys: ownerPhys, dpr: ownerDpr);
    for (final g in [EGroup.light, EGroup.color, EGroup.style, EGroup.crop, EGroup.effects]) {
      await dockTap(t, 'dock-$g');
      await settle(t, 20);
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(t.getRect(find.byType(PhotoArt)).bottom, lessThanOrEqualTo(panel.top + 0.5), reason: '$g');
      expect(t.takeException(), isNull, reason: '$g');
      await dockTap(t, 'dock-$g');
    }
    await dockTap(t, 'dock-${EGroup.color}');
    await chipTap(t, 'chip-color-band');
    await dockTap(t, 'dock-${EGroup.style}');
    await chipTap(t, 'chip-style-palette');
    expect(t.takeException(), isNull);
  });
}
