// Shared controls (guidelines section 10, Android / Material 3 column). iOS variants hook in
// through `DStyle` later; call sites only use these widgets.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/edit_interaction.dart';
import '../state/app_state.dart';
import '../theme/tokens.dart';
import 'icons.dart';

/// Minimum touch target on Android.
const double kHit = 48;

// ---------------------------------------------------------------- buttons

class PrimaryButton extends StatelessWidget {
  const PrimaryButton(this.label, {super.key, this.onPressed, this.danger = false, this.height = 52});
  final String label;
  final VoidCallback? onPressed;
  final bool danger;
  final double height;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      enabled: onPressed != null,
      label: label,
      excludeSemantics: true,
      child: Opacity(
        opacity: onPressed == null ? 0.4 : 1,
        child: Material(
          color: danger ? c.danger : c.accentBtn,
          borderRadius: BorderRadius.circular(Rd.cta),
          child: InkWell(
            borderRadius: BorderRadius.circular(Rd.cta),
            onTap: onPressed,
            child: Container(
              height: height,
              alignment: Alignment.center,
              padding: const EdgeInsets.symmetric(horizontal: 24),
              child: Text(label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body17(Colors.white, w: FontWeight.w500)),
            ),
          ),
        ),
      ),
    );
  }
}

/// Secondary large button (chip fill) or the soft accent variant.
class SecondaryButton extends StatelessWidget {
  const SecondaryButton(this.label, {super.key, this.onPressed, this.soft = false, this.height = 52});
  final String label;
  final VoidCallback? onPressed;
  final bool soft;
  final double height;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      enabled: onPressed != null,
      label: label,
      excludeSemantics: true,
      child: Opacity(
        opacity: onPressed == null ? 0.4 : 1,
        child: Material(
          color: soft ? c.accentSoft : c.chip,
          borderRadius: BorderRadius.circular(Rd.cta),
          child: InkWell(
            borderRadius: BorderRadius.circular(Rd.cta),
            onTap: onPressed,
            child: Container(
              height: height,
              alignment: Alignment.center,
              padding: const EdgeInsets.symmetric(horizontal: 24),
              child: Text(label,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: T.body17(soft ? c.accent : c.text, w: FontWeight.w500)),
            ),
          ),
        ),
      ),
    );
  }
}

/// Small action chip (visual 32, hit 48): accentSoft fill, accent text.
class ActionChip32 extends StatelessWidget {
  const ActionChip32(this.label, {super.key, this.onPressed, this.color});
  final String label;
  final VoidCallback? onPressed;
  final Color? color;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onPressed,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: kHit, minWidth: kHit),
          child: Center(
            child: Container(
              height: 32,
              padding: const EdgeInsets.symmetric(horizontal: 16),
              alignment: Alignment.center,
              decoration: BoxDecoration(color: color ?? c.accentSoft, borderRadius: BorderRadius.circular(16)),
              child: Text(label, style: T.small(c.accent, w: FontWeight.w500)),
            ),
          ),
        ),
      ),
    );
  }
}

/// Plain text button (accent), 48 hit.
class TextAction extends StatelessWidget {
  const TextAction(this.label, {super.key, this.onPressed, this.color, this.size = 15, this.bold = false});
  final String label;
  final VoidCallback? onPressed;
  final Color? color;
  final double size;
  final bool bold;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      enabled: onPressed != null,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onPressed,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: kHit, minWidth: kHit),
          child: Center(
            widthFactor: 1,
            child: Opacity(
              opacity: onPressed == null ? 0.4 : 1,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Text(label,
                    maxLines: 1,
                    style: TextStyle(
                        fontSize: size, color: color ?? c.accent, fontWeight: bold ? FontWeight.w500 : FontWeight.w400)),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class DIconButton extends StatelessWidget {
  const DIconButton(this.icon, {super.key, required this.label, this.onPressed, this.color, this.size = 24, this.pressed, this.bg});
  final DI icon;

  /// Semantics label (every icon button needs a text label).
  final String label;
  final VoidCallback? onPressed;
  final Color? color;
  final double size;

  /// Toggle state (aria-pressed); null = not a toggle.
  final bool? pressed;
  final Color? bg;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final enabled = onPressed != null;
    return Semantics(
      button: true,
      toggled: pressed,
      enabled: enabled,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onPressed,
        child: SizedBox(
          width: kHit,
          height: kHit,
          child: Center(
            child: Opacity(
              opacity: enabled ? 1 : 0.4,
              child: bg == null
                  ? DIcon(icon, size: size, color: color ?? c.text)
                  : Container(
                      width: 40,
                      height: 40,
                      decoration: BoxDecoration(color: bg, shape: BoxShape.circle),
                      alignment: Alignment.center,
                      child: DIcon(icon, size: size, color: color ?? c.text)),
            ),
          ),
        ),
      ),
    );
  }
}

// ---------------------------------------------------------------- chips / segmented / switch / checkbox

/// Filter chip (Android): 32 high, radius 8, outline; selected = secContainer.
class FilterChip32 extends StatelessWidget {
  const FilterChip32(this.label, {super.key, required this.selected, this.onTap, this.leading});
  final String label;
  final bool selected;
  final VoidCallback? onTap;
  final Widget? leading;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      toggled: selected,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: kHit),
          child: Center(
            child: Container(
              height: 32,
              padding: const EdgeInsets.symmetric(horizontal: 12),
              decoration: BoxDecoration(
                color: selected ? c.secContainer : null,
                border: selected ? null : Border.all(color: c.outline),
                borderRadius: BorderRadius.circular(8),
              ),
              child: Row(mainAxisSize: MainAxisSize.min, children: [
                if (selected) ...[DIcon(DI.check, size: 16, color: c.onSecContainer, stroke: 2.4), const SizedBox(width: 6)],
                if (leading != null) ...[leading!, const SizedBox(width: 6)],
                Text(label,
                    style: T.small(selected ? c.onSecContainer : c.text2, w: FontWeight.w500)),
              ]),
            ),
          ),
        ),
      ),
    );
  }
}

/// M3 segmented control (outlined, selected = secContainer + check).
class DSegmented extends StatelessWidget {
  const DSegmented({super.key, required this.labels, required this.index, required this.onChanged, this.semanticsLabel});
  final List<String> labels;
  final int index;
  final ValueChanged<int> onChanged;
  final String? semanticsLabel;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      label: semanticsLabel,
      container: true,
      child: Container(
        height: 40,
        decoration: BoxDecoration(border: Border.all(color: c.outline), borderRadius: BorderRadius.circular(20)),
        clipBehavior: Clip.antiAlias,
        child: Row(children: [
          for (var i = 0; i < labels.length; i++) ...[
            if (i > 0) Container(width: 1, color: c.outline),
            Expanded(
              child: Semantics(
                inMutuallyExclusiveGroup: true,
                checked: i == index,
                label: labels[i],
                excludeSemantics: true,
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTap: () => onChanged(i),
                  child: Container(
                    color: i == index ? c.secContainer : null,
                    alignment: Alignment.center,
                    child: Row(mainAxisSize: MainAxisSize.min, children: [
                      if (i == index) ...[DIcon(DI.check, size: 14, color: c.onSecContainer, stroke: 2.6), const SizedBox(width: 4)],
                      Text(labels[i], style: T.small(i == index ? c.onSecContainer : c.text, w: FontWeight.w500)),
                    ]),
                  ),
                ),
              ),
            ),
          ],
        ]),
      ),
    );
  }
}

class DSwitch extends StatelessWidget {
  const DSwitch({super.key, required this.value, required this.onChanged, this.label});
  final bool value;
  final ValueChanged<bool> onChanged;
  final String? label;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      label: label,
      toggled: value,
      container: true,
      excludeSemantics: true,
      onTap: () => onChanged(!value),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () => onChanged(!value),
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 150),
          width: 52,
          height: 32,
          padding: const EdgeInsets.all(2),
          decoration: BoxDecoration(
            color: value ? c.accentBtn : c.chip,
            borderRadius: BorderRadius.circular(16),
            border: value ? null : Border.all(color: c.outline, width: 2),
          ),
          alignment: value ? Alignment.centerRight : Alignment.centerLeft,
          child: Container(
            width: value ? 24 : 16,
            height: value ? 24 : 16,
            margin: EdgeInsets.all(value ? 2 : 6),
            decoration: BoxDecoration(color: value ? Colors.white : c.outline, shape: BoxShape.circle),
          ),
        ),
      ),
    );
  }
}

/// Checkbox 22x22, radius 2 (Android).
class DCheckbox extends StatelessWidget {
  const DCheckbox({super.key, required this.value, this.size = 22});
  final bool value;
  final double size;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: value ? c.accentBtn : null,
        borderRadius: BorderRadius.circular(2),
        border: value ? null : Border.all(color: c.outline, width: 2),
      ),
      alignment: Alignment.center,
      child: value ? DIcon(DI.check, size: size - 6, color: Colors.white, stroke: 3.4) : null,
    );
  }
}

/// Selection circle on thumbnails (right-top): number when selected, 35% black when not.
class SelectDot extends StatelessWidget {
  const SelectDot({super.key, required this.number, this.size = 26});
  final int number; // 0 = unselected
  final double size;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final on = number > 0;
    return Container(
      width: size,
      height: size,
      alignment: Alignment.center,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: on ? c.accentBtn : Fx.overlay35,
        border: Border.all(color: Colors.white, width: 2),
      ),
      child: on ? Text('$number', style: TextStyle(color: Colors.white, fontSize: size * .5, fontWeight: FontWeight.w600)) : null,
    );
  }
}

// ---------------------------------------------------------------- slider

/// Lightroom-style slider row (guidelines 10.4, Android thumb). Bipolar sliders fill from the
/// zero tick; sliders whose default is not the minimum fill from the default position; gradient
/// tracks show no fill.
class DSlider extends StatefulWidget {
  const DSlider({
    super.key,
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.def,
    required this.display,
    required this.onChanged,
    this.onChangeEnd,
    this.gradient,
    this.step = 1,
    this.keyPrefix,
  });

  final String label;
  final double value, min, max, def;
  final String display;
  final ValueChanged<double> onChanged;
  final ValueChanged<double>? onChangeEnd;
  final List<Color>? gradient;
  final double step;

  /// Used for widget keys in tests: `${keyPrefix}-slider`, `${keyPrefix}-reset`.
  final String? keyPrefix;

  @override
  State<DSlider> createState() => _DSliderState();
}

class _DSliderState extends State<DSlider> {
  double? _lastHaptic;
  bool _dragging = false;

  // Tells the editor engine a drag is in progress (interactive-quality frames, see EditInteraction).
  void _dragBegin() {
    if (_dragging) return;
    _dragging = true;
    EditInteraction.begin();
  }

  void _dragEnd() {
    if (!_dragging) return;
    _dragging = false;
    EditInteraction.end();
  }

  @override
  void dispose() {
    _dragEnd();
    super.dispose();
  }

  bool get _changed => (widget.value - widget.def).abs() > 1e-9;

  double _snap(double v) {
    final s = widget.step;
    final r = (v / s).round() * s;
    return r.clamp(widget.min, widget.max).toDouble();
  }

  void _setFromDx(double dx, double width) {
    final f = (dx / width).clamp(0.0, 1.0);
    final v = _snap(widget.min + f * (widget.max - widget.min));
    // selection haptic at the neutral point and both ends only
    if ((v == widget.def || v == widget.min || v == widget.max) && _lastHaptic != v) {
      HapticFeedback.selectionClick();
    }
    _lastHaptic = v;
    if (v != widget.value) widget.onChanged(v);
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final range = widget.max - widget.min;
    double pos(double v) => ((v - widget.min) / range).clamp(0.0, 1.0);
    final bipolar = widget.min < 0 && widget.def == 0;
    const thumb = 20.0;
    return Semantics(
      container: true,
      slider: true,
      label: widget.label,
      value: widget.display,
      increasedValue: widget.display,
      decreasedValue: widget.display,
      onIncrease: () => widget.onChanged(_snap(widget.value + widget.step)),
      onDecrease: () => widget.onChanged(_snap(widget.value - widget.step)),
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 0),
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          SizedBox(
            height: 28,
            child: Row(children: [
              Expanded(
                child: Text(widget.label,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: T.body(_changed ? c.accent : c.text, w: _changed ? FontWeight.w500 : FontWeight.w400)),
              ),
              Text(widget.display, style: T.body(c.text2, tabular: true)),
              // reset: always occupies its slot, hidden (not removed) when unchanged
              Visibility(
                visible: _changed,
                maintainSize: true,
                maintainAnimation: true,
                maintainState: true,
                child: Semantics(
                  button: true,
                  label: '${widget.label}复位',
                  excludeSemantics: true,
                  child: GestureDetector(
                    key: widget.keyPrefix == null ? null : ValueKey('${widget.keyPrefix}-reset'),
                    behavior: HitTestBehavior.opaque,
                    onTap: () {
                      widget.onChanged(widget.def);
                      widget.onChangeEnd?.call(widget.def);
                    },
                    child: SizedBox(width: kHit, height: kHit, child: Center(child: DIcon(DI.reset, size: 18, color: c.accent))),
                  ),
                ),
              ),
            ]),
          ),
          LayoutBuilder(builder: (context, cons) {
            final w = cons.maxWidth;
            final usable = w - thumb;
            double x(double v) => thumb / 2 + pos(v) * usable;
            return Listener(
              behavior: HitTestBehavior.translucent,
              onPointerDown: (_) => _dragBegin(),
              onPointerUp: (_) => _dragEnd(),
              onPointerCancel: (_) => _dragEnd(),
              child: GestureDetector(
              key: widget.keyPrefix == null ? null : ValueKey('${widget.keyPrefix}-slider'),
              behavior: HitTestBehavior.opaque,
              onTapDown: (d) => _setFromDx(d.localPosition.dx - thumb / 2, usable),
              onHorizontalDragStart: (d) => _setFromDx(d.localPosition.dx - thumb / 2, usable),
              onHorizontalDragUpdate: (d) => _setFromDx(d.localPosition.dx - thumb / 2, usable),
              onHorizontalDragEnd: (_) => widget.onChangeEnd?.call(widget.value),
              onTapUp: (_) => widget.onChangeEnd?.call(widget.value),
              child: SizedBox(
                height: 32,
                width: w,
                child: Stack(clipBehavior: Clip.none, children: [
                  // track
                  Positioned(
                    left: thumb / 2,
                    right: thumb / 2,
                    top: 14,
                    height: 4,
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        borderRadius: BorderRadius.circular(2),
                        color: widget.gradient == null ? c.track : null,
                        gradient: widget.gradient == null ? null : LinearGradient(colors: widget.gradient!),
                      ),
                    ),
                  ),
                  // fill
                  if (widget.gradient == null)
                    Positioned(
                      left: (x(widget.def) < x(widget.value) ? x(widget.def) : x(widget.value)),
                      width: (x(widget.value) - x(widget.def)).abs(),
                      top: 14,
                      height: 4,
                      child: DecoratedBox(decoration: BoxDecoration(color: c.accent, borderRadius: BorderRadius.circular(2))),
                    ),
                  // zero tick (bipolar only), under the track, does not intercept pointers
                  if (bipolar)
                    Positioned(
                      left: x(widget.def) - 1,
                      top: 6,
                      width: 2,
                      height: 20,
                      child: IgnorePointer(
                        child: DecoratedBox(decoration: BoxDecoration(color: c.tick, borderRadius: BorderRadius.circular(1))),
                      ),
                    ),
                  Positioned(
                    left: x(widget.value) - thumb / 2,
                    top: 16 - thumb / 2,
                    width: thumb,
                    height: thumb,
                    child: DecoratedBox(decoration: BoxDecoration(color: c.accent, shape: BoxShape.circle)),
                  ),
                ]),
              ),
            ));
          }),
        ]),
      ),
    );
  }
}

// ---------------------------------------------------------------- toast / snackbar

/// Single-instance Snackbar layer (Android: full width, radius 4, min height 48). Place inside
/// a Stack; [bottom] is computed per screen so it never covers a primary button.
class ToastLayer extends StatefulWidget {
  const ToastLayer({super.key, required this.app, required this.bottom, this.maxWidth});
  final AppState app;
  final double bottom;
  final double? maxWidth;

  @override
  State<ToastLayer> createState() => _ToastLayerState();
}

class _ToastLayerState extends State<ToastLayer> {
  Timer? _t;
  int? _shown;

  @override
  void initState() {
    super.initState();
    widget.app.addListener(_sync);
    _sync();
  }

  @override
  void dispose() {
    widget.app.removeListener(_sync);
    _t?.cancel();
    super.dispose();
  }

  void _sync() {
    final t = widget.app.toast;
    if (t != null && t.id != _shown) {
      _shown = t.id;
      _t?.cancel();
      _t = Timer(t.duration, () => widget.app.clearToast(t.id));
    }
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final t = widget.app.toast;
    return Positioned(
      left: 8,
      right: 8,
      bottom: widget.bottom,
      child: Align(
        alignment: Alignment.bottomCenter,
        child: ConstrainedBox(
          constraints: BoxConstraints(maxWidth: widget.maxWidth ?? double.infinity),
          child: Semantics(
            liveRegion: true,
            container: true,
            label: t?.message,
            child: t == null
                ? const SizedBox.shrink()
                : Material(
                    color: c.snackBg,
                    elevation: 3,
                    borderRadius: BorderRadius.circular(4),
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(minHeight: 48),
                      child: Padding(
                        padding: const EdgeInsets.fromLTRB(16, 8, 8, 8),
                        child: Row(children: [
                          Expanded(child: Text(t.message, style: T.small(c.snackText))),
                          if (t.actionLabel != null)
                            GestureDetector(
                              key: const ValueKey('toast-action'),
                              behavior: HitTestBehavior.opaque,
                              onTap: () {
                                t.onAction?.call();
                                widget.app.clearToast(t.id);
                              },
                              child: ConstrainedBox(
                                constraints: const BoxConstraints(minHeight: kHit, minWidth: kHit),
                                child: Center(
                                    widthFactor: 1,
                                    child: Padding(
                                        padding: const EdgeInsets.symmetric(horizontal: 12),
                                        child: Text(t.actionLabel!, style: T.small(c.snackAct, w: FontWeight.w500)))),
                              ),
                            ),
                        ]),
                      ),
                    ),
                  ),
          ),
        ),
      ),
    );
  }
}

// ---------------------------------------------------------------- misc

/// Status dot with fixed size and an optional white ring.
class Dot extends StatelessWidget {
  const Dot(this.color, {super.key, this.size = 10, this.ring = false, this.hollow = false});
  final Color color;
  final double size;
  final bool ring;
  final bool hollow;

  @override
  Widget build(BuildContext context) => Container(
        width: size,
        height: size,
        decoration: BoxDecoration(
          color: hollow ? Fx.overlay35 : color,
          shape: BoxShape.circle,
          border: ring || hollow ? Border.all(color: Colors.white, width: hollow ? 2 : 1.5) : null,
        ),
      );
}

/// Linear progress bar (determinate), height 4 / 6.
class DProgress extends StatelessWidget {
  const DProgress(this.value, {super.key, this.height = 4});
  final double value;
  final double height;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      label: '进度',
      value: '${(value * 100).round()}%',
      child: ClipRRect(
        borderRadius: BorderRadius.circular(height / 2),
        child: SizedBox(
          height: height,
          child: Stack(children: [
            Positioned.fill(child: ColoredBox(color: c.track)),
            Positioned.fill(
                child: FractionallySizedBox(
                    alignment: Alignment.centerLeft, widthFactor: value.clamp(0.0, 1.0), child: ColoredBox(color: c.accent))),
          ]),
        ),
      ),
    );
  }
}

/// Rotating spinner (3/4 arc). Static when reduced motion is requested.
class Spinner extends StatefulWidget {
  const Spinner({super.key, this.size = 18, this.color});
  final double size;
  final Color? color;
  @override
  State<Spinner> createState() => _SpinnerState();
}

class _SpinnerState extends State<Spinner> with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(vsync: this, duration: const Duration(seconds: 1));
  bool _started = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final reduce = MediaQuery.maybeDisableAnimationsOf(context) ?? false;
    if (reduce) {
      _c.stop();
      _started = false;
    } else if (!_started) {
      _c.repeat();
      _started = true;
    }
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => RotationTransition(
      turns: _c, child: DIcon(DI.spinner, size: widget.size, color: widget.color ?? Colors.white));
}
