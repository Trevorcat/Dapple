// Small widgets shared by the control panel (docs/design/editor-panel.md): the category icons, the mini
// histogram of the value line and the typed-value field. (The floating category windows they used to live
// next to are gone: the panel is under the photo or beside it on every screen.)
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../model/params.dart';
import '../../services/editor_engine.dart';
import '../../theme/tokens.dart';
import '../../widgets/icons.dart';
import 'editor_widgets.dart';

const groupIcons = {
  EGroup.style: DI.styles,
  EGroup.light: DI.light,
  EGroup.color: DI.color,
  EGroup.curve: DI.curve,
  EGroup.effects: DI.effects,
  EGroup.detail: DI.detail,
  EGroup.grade: DI.grade,
  EGroup.crop: DI.crop,
};


class MiniHistogramBox extends StatelessWidget {
  const MiniHistogramBox({super.key, required this.analysis});
  final Analysis analysis;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      image: true,
      label: '直方图，${tonePlain(analysis.nearest)}',
      child: ClipRRect(
        borderRadius: BorderRadius.circular(8),
        child: Container(
          width: 96,
          height: 32,
          color: c.bg,
          child: CustomPaint(painter: HistPainter(analysis, c.histFill, c.text, c.zoneA, c.accent, zones: false)),
        ),
      ),
    );
  }
}


/// The typed-value field (requirements 4.15.10): numeric keyboard with minus, commit on done or
/// when it loses focus (tap on the photo), empty = cancel.
class NumberEntry extends StatefulWidget {
  const NumberEntry({super.key, required this.initial, required this.style, required this.onSubmit});
  final String initial;
  final TextStyle style;
  final ValueChanged<String> onSubmit;
  @override
  State<NumberEntry> createState() => NumberEntryState();
}

class NumberEntryState extends State<NumberEntry> {
  late final TextEditingController _tc = TextEditingController(text: widget.initial);
  final FocusNode _fn = FocusNode();
  bool _done = false;

  @override
  void initState() {
    super.initState();
    _tc.selection = TextSelection(baseOffset: 0, extentOffset: widget.initial.length);
    _fn.addListener(() {
      if (!_fn.hasFocus) _commit();
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _fn.requestFocus();
    });
  }

  void _commit() {
    if (_done) return;
    _done = true;
    widget.onSubmit(_tc.text);
  }

  @override
  void dispose() {
    _fn.dispose();
    _tc.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 76,
      child: TextField(
        controller: _tc,
        focusNode: _fn,
        textAlign: TextAlign.right,
        style: widget.style,
        keyboardType: const TextInputType.numberWithOptions(signed: true, decimal: true),
        inputFormatters: [FilteringTextInputFormatter.allow(RegExp(r'[0-9.\-−+]'))],
        textInputAction: TextInputAction.done,
        onSubmitted: (_) => _commit(),
        decoration: const InputDecoration(isDense: true, border: InputBorder.none, contentPadding: EdgeInsets.symmetric(vertical: 8)),
      ),
    );
  }
}

