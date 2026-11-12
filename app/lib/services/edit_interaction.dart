import 'package:flutter/foundation.dart';

/// Process-wide "a drag on an edit control is in progress" signal.
///
/// Sliders ([DSlider]) call [begin] when the finger goes down and [end] when it lifts or the gesture
/// is cancelled; the editor engine listens to [depth] and renders at interactive quality (smaller,
/// cheaper frames, presented as fast as they finish) while it is non-zero, then renders the
/// full-quality frame once the control is released. Controls that do not call this are still
/// detected: the engine also infers a drag from the rate of `edited()` calls.
class EditInteraction {
  EditInteraction._();

  /// Number of drags currently in progress (normally 0 or 1; counted so overlapping ones balance).
  static final ValueNotifier<int> depth = ValueNotifier<int>(0);

  static void begin() => depth.value = depth.value + 1;

  static void end() {
    if (depth.value > 0) depth.value = depth.value - 1;
  }
}
