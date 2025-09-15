import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

class MpvEmbedWidget extends StatefulWidget {
  final String url;
  const MpvEmbedWidget({super.key, required this.url});

  @override
  State<MpvEmbedWidget> createState() => _MpvEmbedWidgetState();
}

class _MpvEmbedWidgetState extends State<MpvEmbedWidget> {
  static const MethodChannel _platform = MethodChannel('mpv_player');

  int? _textureId;
  bool _isPlaying = false;
  bool _inited = false;
  bool _loading = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _initPlayer();
  }

  Future<void> _initPlayer() async {
    setState(() {
      _loading = true;
      _error = null;
    });

    try {
      // Initialize the native mpv plugin and get texture ID
      final int textureId = await _platform.invokeMethod<int>('init') ?? -1;
      if (textureId < 0) {
        throw Exception('Failed to initialize mpv texture');
      }

      setState(() {
        _textureId = textureId;
        _inited = true;
      });

      // Load the video file
      await _platform.invokeMethod('load', {'url': widget.url});

      // Auto-play the video
      await _play();

      setState(() {
        _loading = false;
      });

      debugPrint(
          'MPV player initialized successfully with texture ID: $textureId');
    } catch (e) {
      setState(() {
        _error = e.toString();
        _loading = false;
      });
      debugPrint('MPV player initialization error: $e');
    }
  }

  @override
  void dispose() {
    _platform.invokeMethod('dispose').catchError((e) {
      debugPrint('Error disposing mpv player: $e');
    });
    super.dispose();
  }

  Future<void> _play() async {
    try {
      await _platform.invokeMethod('play');
      setState(() => _isPlaying = true);
    } catch (e) {
      debugPrint('Error playing video: $e');
    }
  }

  Future<void> _pause() async {
    try {
      await _platform.invokeMethod('pause');
      setState(() => _isPlaying = false);
    } catch (e) {
      debugPrint('Error pausing video: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            CircularProgressIndicator(),
            SizedBox(height: 16),
            Text('Initializing video player...'),
          ],
        ),
      );
    }

    if (_error != null) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Icon(Icons.error, color: Colors.red, size: 48),
            const SizedBox(height: 16),
            Text(
              'Error: $_error',
              style: const TextStyle(color: Colors.red),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 16),
            ElevatedButton(
              onPressed: _initPlayer,
              child: const Text('Retry'),
            ),
          ],
        ),
      );
    }

    if (!_inited || _textureId == null) {
      return const Center(
        child: Text('Player not initialized'),
      );
    }

    return Container(
      color: Colors.black,
      child: Stack(
        children: [
          SizedBox.expand(
            child: Texture(textureId: _textureId!),
          ),
          Positioned(
            bottom: 16,
            left: 16,
            right: 16,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              decoration: BoxDecoration(
                color: Colors.black54,
                borderRadius: BorderRadius.circular(8),
              ),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  IconButton(
                    onPressed: _isPlaying ? _pause : _play,
                    icon: Icon(
                      _isPlaying ? Icons.pause : Icons.play_arrow,
                      color: Colors.white,
                      size: 32,
                    ),
                  ),
                  const SizedBox(width: 16),
                  Text(
                    _isPlaying ? 'Playing' : 'Paused',
                    style: const TextStyle(color: Colors.white),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}