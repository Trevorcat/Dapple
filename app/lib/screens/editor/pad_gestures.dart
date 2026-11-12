// Direct-manipulation plumbing of the panel's controls: a one-finger surface that wins the gesture arena the moment a
// pointer lands (so no scroll view around it can steal the drag) and reports local positions. Every control of the panel
// maps the position it gets *absolutely* (the thing under the finger is the thing that moves): there is no relative,
// drag-anywhere mapping any more.
import 'package:flutter/gestures.dart';
import 'package:flutter/widgets.dart';

/// Wins the arena the moment a pointer lands, then reports the pointer's events.
class ClaimPanRecognizer extends OneSequenceGestureRecognizer {
  ClaimPanRecognizer({super.debugOwner});
  void Function(PointerDownEvent)? onDown;
  void Function(PointerMoveEvent)? onMove;
  void Function(PointerEvent, bool cancelled)? onEnd;
  int? _pointer;

  @override
  void addAllowedPointer(PointerDownEvent event) {
    if (_pointer != null) return; // one finger
    _pointer = event.pointer;
    startTrackingPointer(event.pointer, event.transform);
    resolve(GestureDisposition.accepted);
    onDown?.call(event);
  }

  @override
  void handleEvent(PointerEvent event) {
    if (event.pointer != _pointer) return;
    if (event is PointerMoveEvent) {
      onMove?.call(event);
    } else if (event is PointerUpEvent || event is PointerCancelEvent) {
      _pointer = null;
      onEnd?.call(event, event is PointerCancelEvent);
      stopTrackingPointer(event.pointer);
    }
  }

  @override
  void didStopTrackingLastPointer(int pointer) {
    _pointer = null;
  }

  @override
  String get debugDescription => 'claimPan';
}

/// A surface that reports the first contact, every move and the end of one finger, in local coordinates.
class ClaimSurface extends StatelessWidget {
  const ClaimSurface({super.key, required this.child, this.onDown, this.onMove, this.onEnd});
  final Widget child;
  final void Function(Offset local)? onDown;
  final void Function(Offset local)? onMove;
  final void Function(Offset local, bool cancelled)? onEnd;

  @override
  Widget build(BuildContext context) => RawGestureDetector(
        behavior: HitTestBehavior.opaque,
        gestures: {
          ClaimPanRecognizer: GestureRecognizerFactoryWithHandlers<ClaimPanRecognizer>(
            ClaimPanRecognizer.new,
            (r) => r
              ..onDown = ((e) => onDown?.call(e.localPosition))
              ..onMove = ((e) => onMove?.call(e.localPosition))
              ..onEnd = ((e, c) => onEnd?.call(e.localPosition, c)),
          ),
        },
        child: child,
      );
}
