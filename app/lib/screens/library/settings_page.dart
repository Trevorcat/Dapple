import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../services/ftp_ingest.dart';

import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/export_size.dart';
import '../../widgets/icons.dart';
import 'codec_diagnostics_page.dart';
import 'licenses_page.dart';
import 'page_bar.dart';

/// 设置 tab (requirements 4.13). Android: no cards, accent section titles. Wide: categories + detail.
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key, required this.app, required this.wide});
  final AppState app;
  final bool wide;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

/// Shown on 关于 (keep in step with pubspec.yaml).
const kAppVersion = '1.0.0';

const _cats = ['相机', '相机无线接收', '自动处理', '发布', '编辑', '关于'];

class _SettingsPageState extends State<SettingsPage> {
  int _cat = 0;
  AppState get app => widget.app;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ListenableBuilder(
      listenable: app,
      builder: (context, _) => ColoredBox(
        color: c.bg,
        child: Column(children: [
          const PageBar('设置'),
          Expanded(child: widget.wide ? _wide(context) : _phone(context)),
        ]),
      ),
    );
  }

  void _openCodecDiagnostics() {
    Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => const CodecDiagnosticsPage()));
  }

  Widget _phone(BuildContext context) => ListView(padding: const EdgeInsets.only(bottom: 120), children: [
        for (var i = 0; i < _cats.length; i++) ...[SectionTitle(_cats[i]), ..._section(context, i)],
      ]);

  Widget _wide(BuildContext context) {
    final c = context.c;
    return Row(children: [
      SizedBox(
        width: 240,
        // scrolls: on short (landscape) screens the six categories do not fit
        child: ListView(key: const ValueKey('cat-list'), children: [
          for (var i = 0; i < _cats.length; i++)
            Semantics(
              button: true,
              selected: _cat == i,
              label: _cats[i],
              excludeSemantics: true,
              child: GestureDetector(
                key: ValueKey('cat-$i'),
                behavior: HitTestBehavior.opaque,
                onTap: () => setState(() => _cat = i),
                child: Container(
                  height: 56,
                  margin: const EdgeInsets.symmetric(horizontal: Sp.m, vertical: 2),
                  padding: const EdgeInsets.symmetric(horizontal: Sp.l),
                  alignment: Alignment.centerLeft,
                  decoration: BoxDecoration(
                      color: _cat == i ? c.secContainer : null, borderRadius: BorderRadius.circular(28)),
                  child: Text(_cats[i], style: T.body17(_cat == i ? c.onSecContainer : c.text, w: _cat == i ? FontWeight.w500 : FontWeight.w400)),
                ),
              ),
            ),
        ]),
      ),
      Expanded(child: ListView(children: [const SizedBox(height: Sp.s), ..._section(context, _cat)])),
    ]);
  }

  Widget _row(BuildContext context, String title, {String? sub, Widget? trailing, VoidCallback? onTap, VoidCallback? onLongPress, Widget? leading, String? key}) {
    final c = context.c;
    return InkWell(
      key: key == null ? null : ValueKey(key),
      onTap: onTap,
      onLongPress: onLongPress,
      child: Container(
        constraints: const BoxConstraints(minHeight: 56),
        padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
        child: Row(children: [
          if (leading != null) Padding(padding: const EdgeInsets.only(right: Sp.m), child: leading),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
              Text(title, style: T.body17(c.text)),
              if (sub != null) Text(sub, style: T.small(c.text2)),
            ]),
          ),
          ?trailing,
        ]),
      ),
    );
  }

  List<Widget> _ftpSection(BuildContext context) {
    final c = context.c;
    final s = app.settings;
    final addr = app.ftpAddresses;
    final main = preferredAddress(addr);
    final host = main?.ip ?? '手机 IP';
    final status = !app.ftpRunning
        ? (app.ftpError != null ? '无法启动 · ${app.ftpError}' : '已关闭')
        : app.ftpClients > 0
            ? '相机已连接'
            : main == null
                ? '已开启 · 未连接 Wi-Fi 或热点'
                : '已开启 · 等待相机';
    Widget kv(String k, String v, {String? key, Widget? trailing}) => _row(context, k,
        key: key,
        trailing: Row(mainAxisSize: MainAxisSize.min, children: [
          Text(v, style: T.body(c.text2, tabular: true)),
          ?trailing,
        ]));
    return [
      _row(context, '相机无线接收',
          key: 'ftp-row',
          sub: status,
          leading: Dot(app.ftpRunning ? (app.ftpClients > 0 ? c.okIcon : c.accent) : c.track),
          onTap: () => app.setFtpEnabled(!app.ftpRunning),
          trailing: DSwitch(
              key: const ValueKey('sw-ftp'),
              label: '相机无线接收',
              value: s.ftpEnabled,
              onChanged: (v) => app.setFtpEnabled(v))),
      if (s.ftpEnabled) ...[
        kv('地址', main == null ? '—' : main.ip, key: 'ftp-address'),
        if (addr.length > 1)
          Padding(
            padding: const EdgeInsets.fromLTRB(Sp.l, 0, Sp.l, Sp.s),
            child: Text([for (final a in addr) '${a.ip}（${a.isHotspot ? '热点' : a.iface}）'].join('  '),
                style: T.small(c.text2, tabular: true)),
          ),
        kv('端口', '${s.ftpPort}', key: 'ftp-port'),
        kv('用户名', s.ftpUser, key: 'ftp-user'),
        _row(context, '密码',
            key: 'ftp-pass',
            onTap: () {
              Clipboard.setData(ClipboardData(text: s.ftpPass));
              app.showToast('已复制密码');
            },
            trailing: Row(mainAxisSize: MainAxisSize.min, children: [
              Text(s.ftpPass, style: T.body(c.text2, tabular: true)),
              const SizedBox(width: Sp.s),
              ActionChip32('重新生成', onPressed: () => app.regenerateFtpPassword()),
            ])),
        Container(
          key: const ValueKey('ftp-setup'),
          margin: const EdgeInsets.fromLTRB(Sp.l, Sp.m, Sp.l, 0),
          padding: const EdgeInsets.all(Sp.l),
          decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.card)),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text('α7R V 的 FTP 设置', style: T.body17(c.text, w: FontWeight.w500)),
            const SizedBox(height: Sp.s),
            Text('网络 › FTP 传输功能 › 服务器设置', style: T.small(c.text2)),
            const SizedBox(height: 4),
            Text('主机名 $host　端口 ${s.ftpPort}', style: T.body(c.text, tabular: true)),
            Text('用户 ${s.ftpUser}　密码 ${s.ftpPass}', style: T.body(c.text, tabular: true)),
            Text('被动模式 开　安全协议 关', style: T.body(c.text)),
            const SizedBox(height: Sp.s),
            Text('相机与手机连同一 Wi-Fi，或连手机热点', style: T.small(c.text2)),
          ]),
        ),
      ],
    ];
  }

  List<Widget> _section(BuildContext context, int i) {
    final c = context.c;
    final s = app.settings;
    switch (i) {
      case 0:
        final st = app.connState;
        final dot = switch (st) {
          ConnState.connected => c.okIcon,
          ConnState.interrupted => Fx.amber,
          ConnState.disconnected => Fx.disconnected,
          ConnState.idle => c.track,
        };
        final line = switch (st) {
          ConnState.connected => '已连接 · 无线接收 RAW',
          ConnState.interrupted => '${app.interruptedCount} 张传输中断',
          ConnState.disconnected => '已断开',
          ConnState.idle => '未连接',
        };
        final act = app.connAction;
        if (!app.isDemo) {
          return [
            _row(context, '手机相册',
                sub: app.mediaPermitted ? '当前照片来源' : '未允许访问照片',
                leading: Dot(app.mediaPermitted ? c.okIcon : c.track),
                trailing: app.mediaPermitted ? null : ActionChip32('允许访问', onPressed: () => app.connectAction())),
            _row(context, '相机无线接收',
                key: 'add-camera',
                sub: app.ftpRunning ? (app.ftpClients > 0 ? '相机已连接' : '已开启 · 等待相机') : '未开启',
                leading: Dot(app.ftpRunning ? (app.ftpClients > 0 ? c.okIcon : c.accent) : c.track),
                trailing: null),
          ];
        }
        return [
          _row(context, app.cameraName,
              sub: line,
              leading: Dot(dot),
              trailing: act == null ? null : ActionChip32(act, onPressed: () => app.connectAction())),
          _row(context, '添加相机…', key: 'add-camera', onTap: () => app.showToast('待实现')),
        ];
            case 1:
        return _ftpSection(context);
      case 2:
        return [
          _row(context, '照片到达后自动处理',
              sub: '先按明暗轻微增强，再套用默认风格',
              onTap: () => app.updateSettings((x) => x.autoProcess = !x.autoProcess),
              trailing: DSwitch(
                  key: const ValueKey('sw-auto'),
                  label: '照片到达后自动处理',
                  value: s.autoProcess,
                  onChanged: (v) => app.updateSettings((x) => x.autoProcess = v))),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
            child: DSlider(
              keyPrefix: 'tone',
              label: '默认明暗强度',
              value: s.toneStrength,
              min: 0,
              max: 100,
              def: 60,
              step: 5,
              display: fmtValue(Fmt.pct, s.toneStrength),
              onChanged: (v) => app.updateSettings((x) => x.toneStrength = v),
            ),
          ),
        ];
      case 4:
        return [
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text('惯用手', style: T.body17(c.text)),
              const SizedBox(height: 2),
              Text('浮窗默认停靠在惯用手一侧；自动会根据你的拇指习惯判断', style: T.small(c.text2)),
              const SizedBox(height: Sp.s),
              DSegmented(
                key: const ValueKey('seg-handedness'),
                semanticsLabel: '惯用手',
                labels: const ['自动', '左手', '右手'],
                index: s.handedness,
                onChanged: (i) => app.updateSettings((x) => x.handedness = i),
              ),
            ]),
          ),
          _row(context, '触感反馈',
              sub: '仅在自动位、范围端点和档位处轻震；关闭后仍保留视觉提示',
              onTap: () => app.updateSettings((x) => x.haptics = !x.haptics),
              trailing: DSwitch(
                  key: const ValueKey('sw-haptics'),
                  label: '触感反馈',
                  value: s.haptics,
                  onChanged: (v) => app.updateSettings((x) => x.haptics = v))),
          if (s.handedness == 0)
            _row(context, '重置自动判断',
                key: 'reset-hand',
                onTap: () {
                  app.updateSettings((x) {
                    x.handSide = 1;
                    x.editorLayout = '';
                  });
                  app.showToast('已重置自动判断');
                }),
        ];
      case 5:
        return [
          _row(context, '开源许可',
              key: 'licenses',
              onTap: () => Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => LicensesPage(app: app))),
              trailing: DIcon(DI.forward, size: 16, color: c.text2)),
          _row(context, '编解码器诊断',
              key: 'codec-diagnostics',
              sub: '本机视频编解码器与自检结果',
              onTap: _openCodecDiagnostics,
              trailing: DIcon(DI.forward, size: 16, color: c.text2)),
          // the long-press on the version is kept as a shortcut
          _row(context, '版本',
              key: 'about-version',
              onLongPress: _openCodecDiagnostics,
              trailing: Text(kAppVersion, key: const ValueKey('about-version-value'), style: T.body17(c.text2, w: FontWeight.w400))),
        ];
      default:
        return [
          _row(context, '保存大小',
              key: 'export-size',
              sub: exportSizeDesc(s.exportLongEdge),
              onTap: () async {
                final v = await showExportSizeSheet(context, current: s.exportLongEdge);
                if (v != null) app.updateSettings((x) => x.exportLongEdge = v);
              },
              trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                Text(exportSizeName(s.exportLongEdge),
                    key: const ValueKey('export-size-value'), style: T.body17(c.text2, w: FontWeight.w400)),
                const SizedBox(width: 4),
                DIcon(DI.forward, size: 16, color: c.text2),
              ])),
          _row(context, '导出时去除位置信息',
              onTap: () => app.updateSettings((x) => x.stripLocation = !x.stripLocation),
              trailing: DSwitch(
                  key: const ValueKey('sw-strip'),
                  label: '导出时去除位置信息',
                  value: s.stripLocation,
                  onChanged: (v) => app.updateSettings((x) => x.stripLocation = v))),
          _row(context, '平台预设',
              key: 'presets',
              onTap: () => Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => PresetsPage(app: app))),
              trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                Text('${presets.length} 个', style: T.body17(c.text2)),
                const SizedBox(width: 4),
                DIcon(DI.forward, size: 16, color: c.text2),
              ])),
        ];
    }
  }
}

/// Read-only list of the publish presets (editing is an open item in the requirements).
class PresetsPage extends StatelessWidget {
  const PresetsPage({super.key, required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Scaffold(
      backgroundColor: c.bg,
      body: Column(children: [
        PageBar('平台预设',
            leading: DIconButton(DI.back, label: '返回', onPressed: () => Navigator.of(context).maybePop()),
            trailing: TextAction('完成', bold: true, size: 17, onPressed: () => Navigator.of(context).maybePop())),
        Expanded(
          child: ListView(children: [
            for (final p in presets)
              Container(
                constraints: const BoxConstraints(minHeight: 64),
                padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
                alignment: Alignment.centerLeft,
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                  Text(p.name, style: T.body17(c.text)),
                  Text('${p.spec.split(' · ').first} · ${exportSizeName(p.longEdge ?? app.settings.exportLongEdge)}',
                      style: T.small(c.text2, tabular: true)),
                ]),
              ),
          ]),
        ),
      ]),
    );
  }
}
