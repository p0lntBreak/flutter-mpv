// linux/mpv_player_plugin.c
// Simplified version using pixel buffer texture with proper render callback
#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <locale.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>

#define MPV_CHANNEL "mpv_player"

// Custom pixel buffer texture for mpv frames
typedef struct
{
    FlPixelBufferTexture parent_instance;
    uint8_t *pixels;
    int width;
    int height;
    gboolean frame_ready;
    GMutex pixel_mutex;
} MPVPixelTexture;

typedef struct
{
    FlPixelBufferTextureClass parent_class;
} MPVPixelTextureClass;

G_DEFINE_TYPE(MPVPixelTexture, mpv_pixel_texture, fl_pixel_buffer_texture_get_type())

static gboolean mpv_pixel_texture_copy_pixels(FlPixelBufferTexture *texture,
                                              const uint8_t **out_buffer,
                                              uint32_t *width,
                                              uint32_t *height,
                                              GError **error)
{
    MPVPixelTexture *t = (MPVPixelTexture *)texture;

    g_print("!!! FLUTTER CALLED COPY_PIXELS !!!\n");
    fflush(stdout);

    g_mutex_lock(&t->pixel_mutex);

    if (!t->pixels || !t->frame_ready)
    {
        // Return a small black frame if no video ready
        static uint8_t black[4] = {0, 0, 0, 255};
        *out_buffer = black;
        *width = 1;
        *height = 1;
        g_mutex_unlock(&t->pixel_mutex);
        return TRUE;
    }

    *out_buffer = t->pixels;
    *width = t->width;
    *height = t->height;

    g_mutex_unlock(&t->pixel_mutex);
    return TRUE;
}

static void mpv_pixel_texture_dispose(GObject *object)
{
    MPVPixelTexture *t = (MPVPixelTexture *)object;
    g_mutex_clear(&t->pixel_mutex);
    if (t->pixels)
    {
        g_free(t->pixels);
        t->pixels = NULL;
    }
    G_OBJECT_CLASS(mpv_pixel_texture_parent_class)->dispose(object);
}

static void mpv_pixel_texture_class_init(MPVPixelTextureClass *klass)
{
    FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels = mpv_pixel_texture_copy_pixels;
    G_OBJECT_CLASS(klass)->dispose = mpv_pixel_texture_dispose;
}

static void mpv_pixel_texture_init(MPVPixelTexture *t)
{
    t->pixels = NULL;
    t->width = 1920;
    t->height = 1080;
    t->frame_ready = FALSE;
    g_mutex_init(&t->pixel_mutex);
}

static MPVPixelTexture *mpv_pixel_texture_new(int w, int h)
{
    MPVPixelTexture *t = (MPVPixelTexture *)g_object_new(mpv_pixel_texture_get_type(), NULL);
    t->width = w;
    t->height = h;
    size_t buffer_size = (size_t)w * h * 4;
    t->pixels = g_malloc0(buffer_size);
    t->frame_ready = FALSE;
    g_print("Created texture buffer: %dx%d = %zu bytes\n", w, h, buffer_size);
    return t;
}

// Main player structure
typedef struct
{
    mpv_handle *mpv;
    mpv_render_context *mpv_gl;
    MPVPixelTexture *texture;
    FlTextureRegistrar *registrar;
    int64_t texture_id;
    guint timer_id;
    int frame_w;
    int frame_h;
    gboolean initialized;
    gboolean render_context_ready;
    guint frame_count;
    gboolean render_requested;
} MPVPlayer;

static MPVPlayer *g_player = NULL;

// Forward declarations
static gboolean try_render_frame(gpointer user_data);

// Render callback for mpv - just set a flag, don't render directly
static void render_update_callback(void *cb_ctx)
{
    MPVPlayer *p = (MPVPlayer *)cb_ctx;
    if (!p || !p->texture || !p->render_context_ready)
        return;

    g_print("* MPV wants to render frame *\n");

    // Set flag and schedule render on main thread if not already scheduled
    if (!p->render_requested)
    {
        p->render_requested = TRUE;
        g_idle_add(try_render_frame, p);
    }
}

// Callback to notify Flutter of new frame

static gboolean notify_frame_available(gpointer user_data)
{
    MPVPlayer *p = (MPVPlayer *)user_data;
    if (p && p->registrar && p->texture)
    {
        g_print(">>> Notifying Flutter: frame %u available <<<\n", p->frame_count++);
        fl_texture_registrar_mark_texture_frame_available(p->registrar, FL_TEXTURE(p->texture));
    }
    return G_SOURCE_REMOVE;
}

// Render function - FIXED PARAMETER STRUCTURE
static gboolean try_render_frame(gpointer user_data)
{
    MPVPlayer *p = (MPVPlayer *)user_data;
    if (!p || !p->mpv_gl || !p->texture || !p->render_context_ready)
    {
        g_print("try_render_frame: missing components\n");
        return G_SOURCE_REMOVE;
    }

    p->render_requested = FALSE; // Clear the flag

    g_mutex_lock(&p->texture->pixel_mutex);

    g_print("--- Attempting to render frame %u: %dx%d ---\n",
            p->frame_count, p->frame_w, p->frame_h);

    // FIXED: Correct parameter structure for software rendering
    int sw_size[2] = {p->frame_w, p->frame_h};
    int stride = p->frame_w * 4;
    
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, sw_size},
        {MPV_RENDER_PARAM_SW_FORMAT, (char *)"rgba"},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, p->texture->pixels},
        {0}};

    // Try to render
    int rc = mpv_render_context_render(p->mpv_gl, params);

    if (rc >= 0)
    {
        g_print("Render SUCCESS! Result: %d\n", rc);

        // Check if we got actual video data by examining a few pixels
        uint32_t checksum = 0;
        for (int i = 0; i < 1000; i++)
        {
            checksum += p->texture->pixels[i];
        }
        g_print("First 1000 bytes checksum: %u\n", checksum);

        // If checksum is very low, MPV might not be writing data - add test pattern
        if (checksum < 1000)
        {
            g_print("Low checksum detected, adding test pattern overlay\n");
            // Add a visible test pattern in one corner
            for (int y = 0; y < 100; y++)
            {
                for (int x = 0; x < 100; x++)
                {
                    int idx = (y * p->frame_w + x) * 4;
                    p->texture->pixels[idx + 0] = 255; // R - red square
                    p->texture->pixels[idx + 1] = 0;   // G
                    p->texture->pixels[idx + 2] = 0;   // B
                    p->texture->pixels[idx + 3] = 255; // A
                }
            }
        }

        p->texture->frame_ready = TRUE;
        g_idle_add(notify_frame_available, p);
    }
    else if (rc == MPV_ERROR_INVALID_PARAMETER)
    {
        g_print("Render failed: invalid parameter\n");
    }
    else
    {
        g_print("Render failed with code: %d\n", rc);
    }

    g_mutex_unlock(&p->texture->pixel_mutex);
    return G_SOURCE_REMOVE;
}

// Add this function to debug MPV state
static void debug_mpv_state(MPVPlayer *p)
{
    if (!p || !p->mpv) return;
    
    char *filename = NULL;
    char *playback_abort = NULL;
    int64_t demuxer_cache_duration = 0;
    int64_t demuxer_cache_time = 0;
    
    mpv_get_property(p->mpv, "filename", MPV_FORMAT_STRING, &filename);
    mpv_get_property(p->mpv, "playback-abort", MPV_FORMAT_STRING, &playback_abort);
    mpv_get_property(p->mpv, "demuxer-cache-duration", MPV_FORMAT_INT64, &demuxer_cache_duration);
    mpv_get_property(p->mpv, "demuxer-cache-time", MPV_FORMAT_INT64, &demuxer_cache_time);
    
    g_print("MPV Debug State:\n");
    g_print("  filename: %s\n", filename ? filename : "none");
    g_print("  playback-abort: %s\n", playback_abort ? playback_abort : "none");
    g_print("  cache-duration: %ld\n", demuxer_cache_duration);
    g_print("  cache-time: %ld\n", demuxer_cache_time);
    
    if (filename) mpv_free(filename);
    if (playback_abort) mpv_free(playback_abort);
}

// Timer callback for status monitoring
static gboolean status_timer_cb(gpointer userdata)
{
    MPVPlayer *p = (MPVPlayer *)userdata;
    if (!p || !p->initialized)
        return G_SOURCE_REMOVE;

    static int debug_counter = 0;
    if (debug_counter++ % 30 == 0)
    { // Every second at 30fps
        int paused = 1;
        double time_pos = 0;
        char *filename = NULL;

        if (mpv_get_property(p->mpv, "pause", MPV_FORMAT_FLAG, &paused) >= 0)
        {
            mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);
            mpv_get_property(p->mpv, "filename", MPV_FORMAT_STRING, &filename);

            g_print("Status: paused=%d, time=%.2f, file=%s\n",
                    paused, time_pos, filename ? filename : "none");

            if (filename)
            {
                mpv_free(filename);
            }
            
            // Add detailed debug every 5 seconds
            if (debug_counter % 150 == 0) {
                debug_mpv_state(p);
            }
        }
    }

    // Add this block every second
    int64_t track_count = 0;
    if (mpv_get_property(p->mpv, "track-list/count", MPV_FORMAT_INT64, &track_count) >= 0) {
        g_print("Track count: %ld\n", track_count);
        for (int64_t i = 0; i < track_count; i++) {
            mpv_node node;
            char key[64];
            snprintf(key, sizeof(key), "track-list/%ld/type", i);
            if (mpv_get_property(p->mpv, key, MPV_FORMAT_STRING, &node) >= 0) {
                g_print("Track %ld type: %s\n", i, node.u.string);
                mpv_free(node.u.string);
            }
        }
    }
    
    return G_SOURCE_CONTINUE;
}

// Initialize mpv render context - MOVED TO AFTER FILE LOADING
static void init_mpv_render_context(MPVPlayer *p)
{
    if (p->render_context_ready)
        return;

    g_print("=== Initializing mpv software render context ===\n");

    // CRITICAL: Set vo to null for software rendering
    mpv_set_property_string(p->mpv, "vo", "null");
    mpv_set_property_string(p->mpv, "hwdec", "no");
    mpv_set_property_string(g_player->mpv, "vd-lavc-dr", "no");
    mpv_set_property_string(g_player->mpv, "keep-open", "yes");

    int64_t track_count = 0;
    if (mpv_get_property(g_player->mpv, "track-list/count", MPV_FORMAT_INT64, &track_count) >= 0) {
        g_print("Track count: %ld\n", track_count);
   }
    for (int64_t i = 0; i < track_count; i++) {
        mpv_node node;
        char key[64];
        snprintf(key, sizeof(key), "track-list/%ld/type", i);
        if (mpv_get_property(g_player->mpv, key, MPV_FORMAT_STRING, &node) >= 0) {
            g_print("Track %ld type: %s\n", i, node.u.string);
            mpv_free(node.u.string);
    }
}  
    
    // Create software mpv render context
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW},
        {0}};

    int rc = mpv_render_context_create(&p->mpv_gl, p->mpv, params);
    if (rc < 0)
    {
        g_printerr("mpv_render_context_create failed: %d\n", rc);
        return;
    }

    // Set render update callback
    mpv_render_context_set_update_callback(p->mpv_gl, render_update_callback, p);

    p->render_context_ready = TRUE;
    g_print("=== mpv software render context created successfully ===\n");
}

// Method handlers
static FlMethodResponse *method_init(FlTextureRegistrar *registrar, FlView *view)
{
    if (g_player)
    {
        return FL_METHOD_RESPONSE(fl_method_error_response_new("ALREADY", "already initialized", NULL));
    }

    g_print("=== INITIALIZING MPV PLAYER ===\n");
    setlocale(LC_NUMERIC, "C");

    g_player = g_new0(MPVPlayer, 1);
    g_player->frame_w = 1920;
    g_player->frame_h = 1080;
    g_player->registrar = registrar;
    g_player->render_context_ready = FALSE;
    g_player->frame_count = 0;
    g_player->render_requested = FALSE;

    // Create pixel texture and register with Flutter
    g_player->texture = mpv_pixel_texture_new(g_player->frame_w, g_player->frame_h);
    g_player->texture_id = fl_texture_registrar_register_texture(registrar, FL_TEXTURE(g_player->texture));

    g_print("DEBUG: Texture registered with ID: %ld, texture object: %p\n",
            (long)g_player->texture_id, g_player->texture);
    g_print("DEBUG: Registrar: %p, FL_TEXTURE cast: %p\n",
            registrar, FL_TEXTURE(g_player->texture));
    fflush(stdout);

    // Create mpv instance
    g_player->mpv = mpv_create();
    if (!g_player->mpv)
    {
        g_free(g_player);
        g_player = NULL;
        return FL_METHOD_RESPONSE(fl_method_error_response_new("MPV_CREATE", "mpv_create failed", NULL));
    }

    // Set mpv options BEFORE initialize
    mpv_set_option_string(g_player->mpv, "config", "yes");
    mpv_set_option_string(g_player->mpv, "input-default-bindings", "yes");
    mpv_set_option_string(g_player->mpv, "force-window", "yes");
    // CRITICAL: Don't set vo here - will be set in init_mpv_render_context
    mpv_set_option_string(g_player->mpv, "msg-level", "all=v");

    // Initialize mpv
    if (mpv_initialize(g_player->mpv) < 0)
    {
        mpv_destroy(g_player->mpv);
        g_free(g_player);
        g_player = NULL;
        return FL_METHOD_RESPONSE(fl_method_error_response_new("MPV_INIT", "mpv_initialize failed", NULL));
    }

    // DON'T initialize render context here - wait until after file load
    // init_mpv_render_context(g_player);

    g_player->initialized = TRUE;

    // Start status timer
    g_player->timer_id = g_timeout_add(33, status_timer_cb, g_player);

    g_print("=== mpv player initialized, texture_id: %" G_GINT64_FORMAT " ===\n", g_player->texture_id);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_int(g_player->texture_id)));
}

static FlMethodResponse *method_load(FlValue *args)
{
    if (!g_player || !g_player->mpv)
    {
        return FL_METHOD_RESPONSE(fl_method_error_response_new("NOT_INIT", "player not init", NULL));
    }

    FlValue *urlv = fl_value_lookup_string(args, "url");
    if (!urlv)
    {
        return FL_METHOD_RESPONSE(fl_method_error_response_new("ARG", "missing url", NULL));
    }

    const char *url = fl_value_get_string(urlv);
    g_print("=== Loading URL: %s ===\n", url);

    // Initialize render context BEFORE loading file
   // init_mpv_render_context(g_player);

    // HLS-specific configuration
    if (strstr(url, ".m3u8") != NULL) {
        g_print("HLS stream detected - applying HLS-specific settings\n");
        mpv_set_property_string(g_player->mpv, "hls-bitrate", "max");
        mpv_set_property_string(g_player->mpv, "cache", "yes");
        mpv_set_property_string(g_player->mpv, "demuxer-max-bytes", "50M");
    }

    // Load file
    const char *cmd[] = {"loadfile", url, NULL};
    int rc = mpv_command(g_player->mpv, cmd);
    if (rc < 0)
    {
        g_printerr("mpv load failed: %d\n", rc);
        return FL_METHOD_RESPONSE(fl_method_error_response_new("LOAD", "mpv load failed", NULL));
    }

    // Query video size and resize buffer if needed
    int64_t w = 0, h = 0;
    if (mpv_get_property(g_player->mpv, "width", MPV_FORMAT_INT64, &w) >= 0 &&
        mpv_get_property(g_player->mpv, "height", MPV_FORMAT_INT64, &h) >= 0 &&
        w > 0 && h > 0)
    {
        g_print("Video size: %ld x %ld\n", w, h);
        if (w != g_player->frame_w || h != g_player->frame_h)
        {
            g_mutex_lock(&g_player->texture->pixel_mutex);
            g_free(g_player->texture->pixels);
            g_player->texture->width = w;
            g_player->texture->height = h;
            g_player->frame_w = w;
            g_player->frame_h = h;
            size_t buffer_size = w * h * 4;
            g_player->texture->pixels = g_malloc0(buffer_size);
            g_mutex_unlock(&g_player->texture->pixel_mutex);
        }
    }

    // Initialize render context AFTER loading file and resizing buffer
    init_mpv_render_context(g_player);

    // After mpv_command(g_player->mpv, cmd);
    int found_video = 0;
    for (int tries = 0; tries < 50 && !found_video; tries++) {
        int64_t track_count = 0;
        if (mpv_get_property(g_player->mpv, "track-list/count", MPV_FORMAT_INT64, &track_count) >= 0) {
            for (int64_t i = 0; i < track_count; i++) {
                mpv_node node;
                char key[64];
                snprintf(key, sizeof(key), "track-list/%ld/type", i);
                if (mpv_get_property(g_player->mpv, key, MPV_FORMAT_STRING, &node) >= 0) {
                    if (strcmp(node.u.string, "video") == 0) {
                        found_video = 1;
                    }
                    mpv_free(node.u.string);
                }
            }
        }
        if (!found_video) g_usleep(100000); // sleep 100ms
    }
    if (found_video) {
        init_mpv_render_context(g_player);
    } else {
        g_printerr("No video track found after loading file\n");
    }

    g_print("=== URL loaded successfully ===\n");
    return FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
}

static FlMethodResponse *method_play()
{
    if (!g_player || !g_player->mpv)
    {
        return FL_METHOD_RESPONSE(fl_method_error_response_new("NOT_INIT", "player not init", NULL));
    }

    g_print("=== PLAYING ===\n");
    mpv_set_property_string(g_player->mpv, "pause", "no");
    return FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
}

static FlMethodResponse *method_pause()
{
    if (!g_player || !g_player->mpv)
    {
        return FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
    }

    g_print("=== PAUSING ===\n");
    mpv_set_property_string(g_player->mpv, "pause", "yes");
    return FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
}

static FlMethodResponse *method_dispose()
{
    if (!g_player)
    {
        return FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
    }

    g_print("=== Disposing mpv player ===\n");

    if (g_player->timer_id)
    {
        g_source_remove(g_player->timer_id);
        g_player->timer_id = 0;
    }

    if (g_player->mpv_gl)
    {
        mpv_render_context_free(g_player->mpv_gl);
        g_player->mpv_gl = NULL;
    }

    if (g_player->mpv)
    {
        mpv_destroy(g_player->mpv);
        g_player->mpv = NULL;
    }

    if (g_player->texture)
    {
        g_object_unref(g_player->texture);
        g_player->texture = NULL;
    }

    g_free(g_player);
    g_player = NULL;

    return FL_METHOD_RESPONSE(fl_method_success_response_new(NULL));
}

// Method channel callback
static void method_call_cb(FlMethodChannel *channel, FlMethodCall *call, gpointer user_data)
{
    FlPluginRegistrar *registrar = (FlPluginRegistrar *)user_data;
    FlTextureRegistrar *tex_registrar = fl_plugin_registrar_get_texture_registrar(registrar);
    FlView *view = fl_plugin_registrar_get_view(registrar);

    const gchar *method = fl_method_call_get_name(call);
    FlValue *args = fl_method_call_get_args(call);
    FlMethodResponse *resp = NULL;

    g_print("Method called: %s\n", method);

    if (strcmp(method, "init") == 0)
    {
        resp = method_init(tex_registrar, view);
    }
    else if (strcmp(method, "load") == 0)
    {
        resp = method_load(args);
    }
    else if (strcmp(method, "play") == 0)
    {
        resp = method_play();
    }
    else if (strcmp(method, "pause") == 0)
    {
        resp = method_pause();
    }
    else if (strcmp(method, "dispose") == 0)
    {
        resp = method_dispose();
    }
    else
    {
        resp = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
    }

    fl_method_call_respond(call, resp, NULL);
}

// Plugin registration
void mpv_player_plugin_register_with_registrar(FlPluginRegistrar *registrar)
{
    FlStandardMethodCodec *codec = fl_standard_method_codec_new();
    FlMethodChannel *channel = fl_method_channel_new(
        fl_plugin_registrar_get_messenger(registrar),
        MPV_CHANNEL,
        FL_METHOD_CODEC(codec));

    fl_method_channel_set_method_call_handler(channel, method_call_cb, registrar, NULL);
    g_print("mpv_plugin: registered\n");
}