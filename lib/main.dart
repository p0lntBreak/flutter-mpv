import 'package:flutter/material.dart';
import 'package:flutter_mpv/texture_widget.dart';


void main() {
  WidgetsFlutterBinding.ensureInitialized();

  runApp(MyApp());
}
class MyApp extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Video Player Test',
      theme: ThemeData(
        primarySwatch: Colors.blue,
      ),
      home: Scaffold(
        body: MpvEmbedWidget(
            //url: 'https://live-hls-web-aje.getaj.net/AJE/index.m3u8'),
            url: '/home/point_break/sample.mp4'),
      ),
    );
  }
}
