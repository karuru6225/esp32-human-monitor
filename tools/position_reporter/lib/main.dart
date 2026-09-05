import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';

import 'secrets.dart' as secrets;

void main() => runApp(const PositionReporterApp());

class PositionReporterApp extends StatelessWidget {
  const PositionReporterApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Position Reporter',
      theme: ThemeData(useMaterial3: true, colorSchemeSeed: Colors.teal),
      home: const ReporterPage(),
    );
  }
}

class RefPoint {
  const RefPoint(this.label, this.x, this.y, this.color);
  final String label;
  final double x;
  final double y;
  final Color color;
}

class ReporterPage extends StatefulWidget {
  const ReporterPage({super.key});

  @override
  State<ReporterPage> createState() => _ReporterPageState();
}

class _ReporterPageState extends State<ReporterPage> {
  // PC側 tools/csi_listener の position_listener (UDP) 宛先
  // 実際のIPは secrets.dart (.gitignore済み) に分離。secrets.dart.exampleを参照
  static const String pcIp = secrets.pcIp;
  static const int pcPort = 5006;

  // assets/floorplan.png の実サイズ(289x515)に合わせた縦横比
  static const double mapAspectRatio = 289 / 515;

  // 位置報告アプリで実測した現在の既知配置(2026-09-05時点)。配置を変えたら数値を更新すること
  static const List<RefPoint> refPoints = [
    RefPoint('ルーター', 0.215, 0.434, Colors.red),
    RefPoint('椅子', 0.325, 0.570, Colors.orange),
    RefPoint('A(TX)', 0.852, 0.325, Colors.green),
    RefPoint('B', 0.380, 0.723, Colors.blue),
    RefPoint('C', 0.100, 0.554, Colors.purple),
  ];

  String _status = '未送信';
  final List<String> _history = [];
  Offset? _lastTap;

  Future<void> _send(Map<String, dynamic> fields, String historyLabel) async {
    final now = DateTime.now();
    final payload = jsonEncode({
      ...fields,
      'client_time': now.toIso8601String(),
    });
    final timeStr =
        '${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}:${now.second.toString().padLeft(2, '0')}';
    try {
      final socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
      socket.send(utf8.encode(payload), InternetAddress(pcIp), pcPort);
      socket.close();
      setState(() {
        _status = '送信済み: $historyLabel @ $timeStr';
        _history.insert(0, '$timeStr  $historyLabel');
      });
    } catch (e) {
      setState(() {
        _status = '送信失敗: $e';
      });
    }
  }

  void _onMapTap(TapDownDetails details, BoxConstraints constraints) {
    final x = details.localPosition.dx / constraints.maxWidth;
    final y = details.localPosition.dy / constraints.maxHeight;
    setState(() => _lastTap = Offset(x, y));
    _send(
      {'label': 'pos', 'x': x, 'y': y},
      'x=${x.toStringAsFixed(3)}, y=${y.toStringAsFixed(3)}',
    );
  }

  void _onAway() {
    setState(() => _lastTap = null);
    _send({'label': '不在'}, '不在');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text('位置報告 → $pcIp:$pcPort')),
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          children: [
            AspectRatio(
              aspectRatio: mapAspectRatio,
              child: LayoutBuilder(
                builder: (context, constraints) {
                  return GestureDetector(
                    onTapDown: (d) => _onMapTap(d, constraints),
                    child: Container(
                      decoration: BoxDecoration(
                        border: Border.all(color: Colors.teal, width: 2),
                      ),
                      child: Stack(
                        fit: StackFit.expand,
                        children: [
                          Image.asset('assets/floorplan.png', fit: BoxFit.fill),
                          for (final p in refPoints)
                            Positioned(
                              left: p.x * constraints.maxWidth - 5,
                              top: p.y * constraints.maxHeight - 5,
                              child: Container(
                                width: 10,
                                height: 10,
                                decoration: BoxDecoration(
                                  color: p.color,
                                  shape: BoxShape.circle,
                                  border: Border.all(color: Colors.white, width: 1),
                                ),
                              ),
                            ),
                          for (final p in refPoints)
                            Positioned(
                              left: p.x * constraints.maxWidth + 7,
                              top: p.y * constraints.maxHeight - 8,
                              child: Text(
                                p.label,
                                style: TextStyle(
                                  fontSize: 11,
                                  fontWeight: FontWeight.bold,
                                  color: p.color,
                                  backgroundColor: Colors.white.withValues(alpha: 0.7),
                                ),
                              ),
                            ),
                          if (_lastTap != null)
                            Positioned(
                              left: _lastTap!.dx * constraints.maxWidth - 12,
                              top: _lastTap!.dy * constraints.maxHeight - 24,
                              child: const Icon(
                                Icons.person_pin_circle,
                                color: Colors.redAccent,
                                size: 24,
                              ),
                            ),
                        ],
                      ),
                    ),
                  );
                },
              ),
            ),
            const SizedBox(height: 16),
            SizedBox(
              width: double.infinity,
              height: 56,
              child: ElevatedButton.icon(
                onPressed: _onAway,
                icon: const Icon(Icons.logout),
                label: const Text('不在', style: TextStyle(fontSize: 20)),
              ),
            ),
            const SizedBox(height: 16),
            Text(_status, style: Theme.of(context).textTheme.titleMedium),
            const Divider(height: 32),
            Expanded(
              child: ListView.builder(
                itemCount: _history.length,
                itemBuilder: (context, i) => Text(
                  _history[i],
                  style: const TextStyle(fontFamily: 'monospace'),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
