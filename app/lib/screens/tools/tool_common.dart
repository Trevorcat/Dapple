import 'package:flutter/material.dart';

import '../../theme/tokens.dart';
import '../../widgets/controls.dart';

/// Full-screen tool layout (requirements 4.9.1): 取消 / title / spacer on top, scrolling body,
/// fixed main button with a hairline above it.
class ToolScaffold extends StatelessWidget {
  const ToolScaffold({
    super.key,
    required this.title,
    required this.body,
    required this.mainLabel,
    required this.onMain,
    required this.onCancel,
  });
  final String title;
  final Widget body;
  final String mainLabel;
  final VoidCallback? onMain;
  final VoidCallback onCancel;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final top = MediaQuery.paddingOf(context).top;
    final bottom = MediaQuery.paddingOf(context).bottom;
    return Scaffold(
      backgroundColor: c.bg,
      body: Column(children: [
        Container(
          height: top + 56,
          padding: EdgeInsets.only(top: top, left: 4, right: 4),
          child: Row(children: [
            SizedBox(width: 72, child: Align(alignment: Alignment.centerLeft, child: TextAction('取消', size: 17, onPressed: onCancel))),
            Expanded(child: Text(title, textAlign: TextAlign.center, style: T.body17(c.text, w: FontWeight.w600))),
            const SizedBox(width: 72),
          ]),
        ),
        Expanded(child: Center(child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 640), child: body))),
        Container(
          decoration: BoxDecoration(border: Border(top: BorderSide(color: c.sep))),
          padding: EdgeInsets.fromLTRB(Sp.l, Sp.m, Sp.l, Sp.m + bottom),
          child: Center(
              child: ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 640),
                  child: KeyedSubtree(key: const ValueKey('tool-main'), child: PrimaryButton(mainLabel, onPressed: onMain)))),
        ),
      ]),
    );
  }
}

/// Rounded preview box with the "预览" tag at the bottom right.
class PreviewBox extends StatelessWidget {
  const PreviewBox({super.key, required this.child, this.tag = '预览', this.busy = false, this.badge});
  final Widget child;
  final String tag;
  final bool busy;

  /// Top-left badge (Motion Photo).
  final String? badge;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ClipRRect(
      borderRadius: BorderRadius.circular(Rd.tile),
      child: Stack(fit: StackFit.expand, children: [
        ColoredBox(color: c.placeholder, child: child),
        if (busy) const Center(child: Spinner(size: 28)),
        if (badge != null)
          Positioned(left: 10, top: 10, child: _Tag(badge!)),
        Positioned(right: 10, bottom: 10, child: _Tag(tag)),
      ]),
    );
  }
}

class _Tag extends StatelessWidget {
  const _Tag(this.text);
  final String text;

  @override
  Widget build(BuildContext context) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
        decoration: BoxDecoration(color: Fx.overlay60, borderRadius: BorderRadius.circular(12)),
        child: Text(text, style: T.cap(Colors.white, w: FontWeight.w500, tabular: true)),
      );
}

/// Settings block of the tool pages: surface, radius 12, padding 16.
class ToolBlock extends StatelessWidget {
  const ToolBlock({super.key, required this.children});
  final List<Widget> children;

  @override
  Widget build(BuildContext context) => Container(
        margin: const EdgeInsets.symmetric(horizontal: Sp.l),
        padding: const EdgeInsets.all(Sp.l),
        decoration: BoxDecoration(color: context.c.surface, borderRadius: BorderRadius.circular(Rd.card)),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: children),
      );
}

class ToolSwitchRow extends StatelessWidget {
  const ToolSwitchRow({super.key, required this.label, required this.value, required this.onChanged, this.switchKey});
  final String label;
  final bool value;
  final ValueChanged<bool> onChanged;
  final Key? switchKey;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return SizedBox(
      height: 48,
      child: Row(children: [
        Expanded(child: Text(label, style: T.body17(c.text))),
        DSwitch(key: switchKey, label: label, value: value, onChanged: onChanged),
      ]),
    );
  }
}
