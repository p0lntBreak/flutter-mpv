///linux/mpv_player_plugin.h
#ifndef MPV_PLAYER_PLUGIN_H_
#define MPV_PLAYER_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

void mpv_player_plugin_register_with_registrar(FlPluginRegistrar* registrar);

G_END_DECLS

#endif  // MPV_PLAYER_PLUGIN_H_