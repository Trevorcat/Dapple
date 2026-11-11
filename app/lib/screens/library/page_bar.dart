import 'package:flutter/material.dart';

import '../../theme/tokens.dart';

/// Android top bar of the top-level pages (status bar + 64, title 22/28 w400).
class PageBar extends StatelessWidget {
  const PageBar(this.title, {super.key, this.leading, this.trailing});
  final String title;
  final Widget? leading;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final top = MediaQuery.paddingOf(context).top;
    return Container(
      color: c.bg,
      height: top + 64,
      padding: EdgeInsets.only(top: top, left: leading == null ? Sp.l : 4, right: 4),
      child: Row(children: [
        ?leading,
        Expanded(child: Text(title, style: T.title(c.text))),
        ?trailing,
      ]),
    );
  }
}

/// Section title: 13/500 accent on Android.
class SectionTitle extends StatelessWidget {
  const SectionTitle(this.text, {super.key});
  final String text;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.fromLTRB(Sp.l, Sp.xl, Sp.l, Sp.s),
        child: Text(text, style: T.small(context.c.accent, w: FontWeight.w500)),
      );
}
