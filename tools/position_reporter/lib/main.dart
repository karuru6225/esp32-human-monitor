import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';

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

class ReporterPage extends StatefulWidget {
  const ReporterPage({super.key});

  @override
  State<ReporterPage> createState() => _ReporterPageState();
}

class _ReporterPageState extends State<ReporterPage> {
  // PC側 tools/csi_listener の position_listener (UDP) 宛先
  // 環境に合わせて自分のPCのLAN IPに書き換えてからビルドすること
  static const String pcIp = '192.168.x.x';
  static const int pcPort = 5006;

  String _status = '未送信';
  final List<String> _history = [];

  Future<void> _send(String label) async {
    final now = DateTime.now();
    final payload = jsonEncode({
      'label': label,
      'client_time': now.toIso8601String(),
    });
    final timeStr =
        '${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}:${now.second.toString().padLeft(2, '0')}';
    try {
      final socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
      socket.send(utf8.encode(payload), InternetAddress(pcIp), pcPort);
      socket.close();
      setState(() {
        _status = '送信済み: $label @ $timeStr';
        _history.insert(0, '$timeStr  $label');
      });
    } catch (e) {
      setState(() {
        _status = '送信失敗: $e';
      });
    }
  }

  Widget _locButton(String label, IconData icon) {
    return SizedBox(
      width: 150,
      height: 110,
      child: ElevatedButton(
        onPressed: () => _send(label),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(icon, size: 34),
            const SizedBox(height: 8),
            Text(label, style: const TextStyle(fontSize: 20)),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('位置報告 → $pcIp:$pcPort')),
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          children: [
            Wrap(
              spacing: 16,
              runSpacing: 16,
              alignment: WrapAlignment.center,
              children: [
                _locButton('椅子', Icons.chair_alt),
                _locButton('ベッド', Icons.bed),
                _locButton('玄関', Icons.sensor_door),
                _locButton('不在', Icons.logout),
              ],
            ),
            const SizedBox(height: 20),
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
