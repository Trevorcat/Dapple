import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../platform/usb_camera.dart';
import '../../services/usb_ingest.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../library/page_bar.dart';

/// Opens the list of the card's photos (the card's 导入照片… action).
void openUsbPicker(BuildContext context, AppState app) {
  Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => UsbPickerPage(app: app)));
}

/// The photos on the camera's card, newest first, loaded a page at a time. Nothing is imported until the user picks and confirms.
class UsbPickerPage extends StatefulWidget {
  const UsbPickerPage({super.key, required this.app});
  final AppState app;

  @override
  State<UsbPickerPage> createState() => _UsbPickerPageState();
}

class _UsbPickerPageState extends State<UsbPickerPage> {
  static const _pageSize = 60;
  final List<UsbGroup> _groups = [];
  final Map<String, UsbGroup> _byGid = {};
  final List<String> _picked = []; // gids in pick order
  final ScrollController _scroll = ScrollController();
  int _next = 0;
  bool _done = false;
  bool _loading = false;
  bool _failed = false;
  int _skippedVideos = 0;

  AppState get app => widget.app;
  UsbIngest get usb => app.usb;

  @override
  void initState() {
    super.initState();
    _scroll.addListener(() {
      if (_scroll.position.extentAfter < 600) _loadMore();
    });
    _loadMore();
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  Future<void> _loadMore() async {
    if (_loading || _done || usb.phase != UsbPhase.ready) return;
    setState(() {
      _loading = true;
      _failed = false;
    });
    try {
      final page = await app.usbBackend.list(cursor: _next, limit: _pageSize);
      if (!mounted) return;
      setState(() {
        for (final g in page.groups) {
          final have = _byGid[g.gid];
          if (have != null) {
            have.merge(g); // the other half of a RAW + JPEG pair that landed on the previous page
          } else {
            _byGid[g.gid] = g;
            _groups.add(g);
          }
        }
        _next = page.next;
        _done = page.done;
        _skippedVideos = page.skippedVideos;
      });
    } catch (_) {
      if (mounted) setState(() => _failed = true);
    } finally {
      if (mounted) setState(() => _loading = false);
    }
    // a page of only videos or folders: keep going until something shows or the card ends
    if (mounted && !_done && !_failed && _groups.isEmpty) _loadMore();
  }

  bool _known(UsbGroup g) => g.files.every(usb.isKnown);

  void _toggle(UsbGroup g) {
    if (_known(g)) return;
    setState(() => _picked.contains(g.gid) ? _picked.remove(g.gid) : _picked.add(g.gid));
  }

  void _import() {
    final chosen = [for (final id in _picked) ?_byGid[id]];
    final n = usb.importGroups(chosen);
    Navigator.of(context).maybePop();
    if (n == 0) app.showToast('已在照片里');
  }

  Future<void> _copyDiagnostics() async {
    final text = await usb.diagnostics();
    await Clipboard.setData(ClipboardData(text: text));
    app.showToast('已复制诊断信息');
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Scaffold(
      key: const ValueKey('usb-picker'),
      backgroundColor: c.bg,
      body: ListenableBuilder(
        listenable: usb,
        builder: (context, _) {
          final gone = usb.phase != UsbPhase.ready;
          return Column(children: [
            PageBar('卡里的照片',
                leading: DIconButton(DI.back, label: '返回', onPressed: () => Navigator.of(context).maybePop()),
                trailing: TextAction('复制诊断信息', size: 15, onPressed: _copyDiagnostics)),
            if (gone)
              Padding(
                padding: const EdgeInsets.fromLTRB(Sp.l, 0, Sp.l, Sp.s),
                child: Align(alignment: Alignment.centerLeft, child: Text(usb.cardTitle, style: T.body(c.warn))),
              ),
            Expanded(child: _grid(context, c)),
            _bottom(context, gone),
          ]);
        },
      ),
    );
  }

  Widget _grid(BuildContext context, DC c) {
    if (_groups.isEmpty) {
      return Center(
        child: Text(_loading ? '读取中' : (_failed ? '读取失败' : '卡里没有照片'), style: T.body(c.text2)),
      );
    }
    return CustomScrollView(controller: _scroll, slivers: [
      SliverPadding(
        padding: const EdgeInsets.symmetric(horizontal: Sp.l),
        sliver: SliverGrid(
          gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(maxCrossAxisExtent: 128, mainAxisSpacing: 4, crossAxisSpacing: 4),
          delegate: SliverChildBuilderDelegate(
            (context, i) {
              final g = _groups[i];
              return _Tile(
                key: ValueKey('usb-tile-${g.gid}'),
                group: g,
                backend: app.usbBackend,
                known: _known(g),
                number: _picked.indexOf(g.gid) + 1,
                onTap: () => _toggle(g),
              );
            },
            childCount: _groups.length,
          ),
        ),
      ),
      SliverToBoxAdapter(
        child: Padding(
          padding: const EdgeInsets.all(Sp.l),
          child: Column(children: [
            if (_loading) const Spinner(),
            if (_failed) TextAction('重试', onPressed: _loadMore),
            if (_skippedVideos > 0) Text('视频暂不导入', style: T.small(c.text2)),
          ]),
        ),
      ),
    ]);
  }

  Widget _bottom(BuildContext context, bool gone) {
    final pad = MediaQuery.paddingOf(context).bottom;
    return Padding(
      padding: EdgeInsets.fromLTRB(Sp.l, Sp.s, Sp.l, Sp.l + pad),
      child: PrimaryButton(
        _picked.isEmpty ? '选择照片' : '导入 ${_picked.length} 张',
        key: const ValueKey('usb-import'),
        onPressed: _picked.isEmpty || gone ? null : _import,
      ),
    );
  }
}

class _Tile extends StatefulWidget {
  const _Tile({super.key, required this.group, required this.backend, required this.known, required this.number, required this.onTap});
  final UsbGroup group;
  final UsbCameraBackend backend;
  final bool known;
  final int number;
  final VoidCallback onTap;

  @override
  State<_Tile> createState() => _TileState();
}

class _TileState extends State<_Tile> {
  Uint8List? _thumb;

  @override
  void initState() {
    super.initState();
    // built lazily by the grid: only tiles near the screen ask the camera for a thumbnail
    widget.backend.thumbnail(widget.group.thumbSource.handle).then((b) {
      if (mounted && b != null && b.isNotEmpty) setState(() => _thumb = b);
    });
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final g = widget.group;
    return Semantics(
      button: true,
      selected: widget.number > 0,
      label: '${g.label}${g.hasRaw ? '，原片' : ''}${widget.known ? '，已在照片里' : ''}',
      excludeSemantics: true,
      child: GestureDetector(
        onTap: widget.onTap,
        child: ClipRRect(
          borderRadius: BorderRadius.circular(Rd.thumb),
          child: Stack(fit: StackFit.expand, children: [
            ColoredBox(color: c.surface),
            if (_thumb != null) Image.memory(_thumb!, fit: BoxFit.cover, gaplessPlayback: true, errorBuilder: (_, _, _) => const SizedBox()),
            if (_thumb == null) Center(child: Text(g.label, style: T.small(c.text2), textAlign: TextAlign.center)),
            if (widget.known) ColoredBox(color: c.bg.withValues(alpha: 0.6)),
            Positioned(
              left: 6,
              bottom: 6,
              child: Text(widget.known ? '已导入' : (g.hasRaw ? 'RAW' : ''), style: T.small(widget.known ? c.text2 : (_thumb == null ? c.text2 : Colors.white), w: FontWeight.w500)),
            ),
            if (!widget.known) Positioned(right: 6, top: 6, child: SelectDot(number: widget.number, size: 24)),
          ]),
        ),
      ),
    );
  }
}
