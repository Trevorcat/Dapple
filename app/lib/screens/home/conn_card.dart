import 'package:flutter/material.dart';

import '../../models.dart';
import '../../services/usb_ingest.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import 'usb_picker_page.dart';

/// Connection card (requirements 4.2.2): dot, title, status line, optional action, progress bar.
class ConnCard extends StatelessWidget {
  const ConnCard({super.key, required this.app});
  final AppState app;

  /// A camera on the cable: the ready card opens the list of the card's photos; the other wired states ask again.
  void _act(BuildContext context) {
    if (app.usbCardActive && app.usb.phase == UsbPhase.ready) {
      openUsbPicker(context, app);
    } else {
      app.connectAction();
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final st = app.connState;
    final dot = switch (st) {
      ConnState.connected => c.okIcon,
      ConnState.interrupted => Fx.amber,
      ConnState.disconnected => Fx.disconnected,
      ConnState.idle => c.track,
    };
    final wired = app.usbCardActive;
    final prog = wired ? app.usb.cardProgress : (st == ConnState.connected ? app.arrivingProgress : null);
    final action = app.connAction;
    return Semantics(
      container: true,
      label: '${app.connTitle}，${app.connLine}',
      child: Container(
        key: const ValueKey('conn-card'),
        margin: const EdgeInsets.symmetric(horizontal: Sp.l),
        decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.card)),
        clipBehavior: Clip.antiAlias,
        // the whole card does what its action chip does (the chip alone is a small target)
        child: Material(
          color: Colors.transparent,
          child: InkWell(
            key: const ValueKey('conn-card-tap'),
            onTap: action == null ? null : () => _act(context),
            child: Container(
              constraints: const BoxConstraints(minHeight: 64),
              padding: const EdgeInsets.fromLTRB(Sp.l, Sp.m, Sp.s, Sp.m),
              child: Row(children: [
                Padding(padding: const EdgeInsets.only(right: Sp.m), child: Dot(dot)),
                Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                    Text(app.connTitle, style: T.body17(c.text)),
                    const SizedBox(height: 2),
                    Text(app.connLine, style: T.body(st == ConnState.interrupted ? c.warn : c.text2, tabular: true)),
                    if (prog != null) ...[const SizedBox(height: 8), Padding(padding: const EdgeInsets.only(right: Sp.s), child: DProgress(prog))],
                  ]),
                ),
                if (action != null) KeyedSubtree(key: const ValueKey('conn-action'), child: ActionChip32(action, onPressed: () => _act(context))),
              ]),
            ),
          ),
        ),
      ),
    );
  }
}
