// The typed-value field of the panel's ruler (docs/design/editor-panel.md section 5).
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

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

