import 'package:flutter/material.dart';

import 'screens/home/shell.dart';
import 'screens/tools/launcher.dart';
import 'state/app_state.dart';
import 'theme/tokens.dart';

/// Root widget. Light first, follows the system dark mode (no in-app switch, no dynamic colour).
class DappleApp extends StatelessWidget {
  const DappleApp({super.key, required this.state, this.forceBrightness});
  final AppState state;

  /// Tests / screenshots only.
  final Brightness? forceBrightness;

  @override
  Widget build(BuildContext context) {
    state.reopenTool = (job) => reopenToolForJob(state, job);
    return MaterialApp(
      navigatorKey: state.navigatorKey,
      title: 'Dapple',
      debugShowCheckedModeBanner: false,
      themeMode: forceBrightness == null
          ? ThemeMode.system
          : (forceBrightness == Brightness.dark ? ThemeMode.dark : ThemeMode.light),
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      home: DappleShell(app: state),
    );
  }
}
