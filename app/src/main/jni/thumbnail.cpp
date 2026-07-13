#include <stdlib.h>
#include <string>
#include <mutex>
#include <stdint.h>
#include <chrono>
#include <unordered_map>

#include <jni.h>
#include <android/bitmap.h>
#include <mpv/client.h>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
    #include <libavcodec/jni.h>
};

#include "jni_utils.h"
#include "globals.h"
#include "log.h"

extern "C" {
    jni_func(jobject, grabThumbnail, jint dimension);
    jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension, jboolean use_hw_dec);
    jni_func(jobject, grabAttachedPicture, jstring jpath, jint dimension);
    jni_func(void, setThumbnailJavaVM, jobject appctx);
    jni_func(void, clearThumbnailCache);
};

// ============================================================================
// MPV-BASED THUMBNAIL GENERATION
// Takes a snapshot of the currently playing video in MPV
// ============================================================================

static inline mpv_node make_node_str(const char *s)
{
    mpv_node r{};
    r.format = MPV_FORMAT_STRING;
    r.u.string = const_cast<char*>(s);
    return r;
}

jni_func(jobject, grabThumbnail, jint dimension) {
    auto total_start = std::chrono::high_resolution_clock::now();
    CHECK_MPV_INIT();
    init_methods_cache(env);

    mpv_node result{};
    {
        mpv_node c{}, c_args[2];
        mpv_node_list c_array{};
        c_args[0] = make_node_str("screenshot-raw");
        c_args[1] = make_node_str("video");
        c_array.num = 2;
        c_array.values = c_args;
        c.format = MPV_FORMAT_NODE_ARRAY;
        c.u.list = &c_array;
        
        if (mpv_command_node(g_mpv, &c, &result) < 0) {
            ALOGE("Thumbnail (MPV) | Screenshot failed");
            return NULL;
        }
    }
    int w = 0, h = 0, stride = 0;
    bool format_ok = false;
    struct mpv_byte_array *data = NULL;
    do {
        if (result.format != MPV_FORMAT_NODE_MAP)
            break;
        for (int i = 0; i < result.u.list->num; i++) {
            std::string key(result.u.list->keys[i]);
            const mpv_node *val = &result.u.list->values[i];
            if (key == "w" || key == "h" || key == "stride") {
                if (val->format != MPV_FORMAT_INT64)
                    break;
                if (key == "w")
                    w = val->u.int64;
                else if (key == "h")
                    h = val->u.int64;
                else
                    stride = val->u.int64;
            } else if (key == "format") {
                if (val->format != MPV_FORMAT_STRING)
                    break;
                format_ok = !strcmp(val->u.string, "bgr0");
            } else if (key == "data") {
                if (val->format != MPV_FORMAT_BYTE_ARRAY)
                    break;
                data = val->u.ba;
            }
        }
    } while (0);
    if (!w || !h || !stride || !format_ok || !data) {
        ALOGE("Thumbnail (MPV) | Failed to extract frame data");
        mpv_free_node_contents(&result);
        return NULL;
    }

    // Crop to square
    int crop_left = 0, crop_top = 0;
    int new_w = w, new_h = h;
    if (w > h) {
        crop_left = (w - h) / 2;
        new_w = h;
    } else if (h > w) {
        crop_top = (h - w) / 2;
        new_h = w;
    }

    uint8_t *new_data = reinterpret_cast<uint8_t*>(data->data);
    new_data += crop_left * sizeof(uint32_t);
    new_data += stride * crop_top;

    // Scale to target size
    struct SwsContext *ctx = sws_getContext(
        new_w, new_h, AV_PIX_FMT_BGR0,
        dimension, dimension, AV_PIX_FMT_RGB32,
        SWS_BICUBIC, NULL, NULL, NULL);
    if (!ctx) {
        ALOGE("Thumbnail (MPV) | Failed to create scaler");
        mpv_free_node_contents(&result);
        return NULL;
    }

    jintArray arr = env->NewIntArray(dimension * dimension);
    jint *scaled = env->GetIntArrayElements(arr, NULL);

    uint8_t *src_p[4] = { new_data }, *dst_p[4] = { (uint8_t*) scaled };
    int src_stride[4] = { stride },
        dst_stride[4] = { (int) sizeof(jint) * dimension };
    
    sws_scale(ctx, src_p, src_stride, 0, new_h, dst_p, dst_stride);
    sws_freeContext(ctx);
    mpv_free_node_contents(&result);
    env->ReleaseIntArrayElements(arr, scaled, 0);

    jobject bitmap_config = env->GetStaticObjectField(android_graphics_Bitmap_Config, android_graphics_Bitmap_Config_ARGB_8888);
    jobject bitmap = env->CallStaticObjectMethod(android_graphics_Bitmap, android_graphics_Bitmap_createBitmap,
        arr, dimension, dimension, bitmap_config);
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(bitmap_config);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start);
    ALOGI("Thumbnail (MPV) | %lldms", (long long)total_duration.count());

    return bitmap;
}

// ============================================================================
// FAST THUMBNAIL GENERATION USING DIRECT FFMPEG API
// Bypasses MPV entirely, uses FFmpeg API directly
// Expected performance: 50-100ms per thumbnail
// ============================================================================

static JavaVM *g_thumb_vm = nullptr;
static jobject g_thumb_appctx = nullptr;
static std::mutex g_thumb_mutex;

// Codec cache for faster initialization
struct CodecCacheEntry {
    AVCodecID codec_id;
    const AVCodec *codec;
    std::chrono::steady_clock::time_point last_used;
};

static std::unordered_map<AVCodecID, CodecCacheEntry> g_codec_cache;
static std::mutex g_codec_cache_mutex;

// Hardware device context cache (expensive to create)
static AVBufferRef *g_hw_device_ctx = nullptr;
static std::mutex g_hw_ctx_mutex;
static bool g_hw_ctx_initialized = false;
static bool g_hw_ctx_available = false;

// Get codec from cache or find it
static const AVCodec* get_cached_codec(AVCodecID codec_id) {
    std::lock_guard<std::mutex> lock(g_codec_cache_mutex);
    
    auto it = g_codec_cache.find(codec_id);
    if (it != g_codec_cache.end()) {
        it->second.last_used = std::chrono::steady_clock::now();
        ALOGV("Thumbnail | Codec found in cache: %s", avcodec_get_name(codec_id));
        return it->second.codec;
    }
    
    // Not in cache, find it
    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (codec) {
        g_codec_cache[codec_id] = {codec_id, codec, std::chrono::steady_clock::now()};
        ALOGV("Thumbnail | Codec added to cache: %s", codec->name);
    }
    
    return codec;
}

// Initialize hardware device context once and reuse it
static bool init_hw_device_context() {
    std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
    
    if (g_hw_ctx_initialized) {
        return g_hw_ctx_available;
    }
    
    g_hw_ctx_initialized = true;
    
    enum AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("mediacodec");
    if (hw_type == AV_HWDEVICE_TYPE_NONE) {
        ALOGD("Thumbnail | MediaCodec not found, HW accel unavailable");
        g_hw_ctx_available = false;
        return false;
    }
    
    if (av_hwdevice_ctx_create(&g_hw_device_ctx, hw_type, NULL, NULL, 0) < 0) {
        ALOGD("Thumbnail | Failed to create HW device context");
        g_hw_ctx_available = false;
        return false;
    }
    
    ALOGI("Thumbnail | Hardware device context initialized successfully");
    g_hw_ctx_available = true;
    return true;
}

// Automatic cleanup on library unload
static void cleanup_thumbnail_resources() __attribute__((destructor));
static void cleanup_thumbnail_resources() {
    // Clear codec cache
    {
        std::lock_guard<std::mutex> lock(g_codec_cache_mutex);
        g_codec_cache.clear();
    }
    
    // Release hardware context
    {
        std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
        if (g_hw_device_ctx) {
            av_buffer_unref(&g_hw_device_ctx);
            g_hw_device_ctx = nullptr;
        }
    }
    
    // Release JNI global references
    {
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        if (g_thumb_appctx && g_thumb_vm) {
            JNIEnv* env = nullptr;
            if (g_thumb_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
                env->DeleteGlobalRef(g_thumb_appctx);
                g_thumb_appctx = nullptr;
            }
        }
    }
}

jni_func(void, setThumbnailJavaVM, jobject appctx) {
    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    
    if (g_thumb_appctx) {
        env->DeleteGlobalRef(g_thumb_appctx);
        g_thumb_appctx = nullptr;
    }
    
    if (!env->GetJavaVM(&g_thumb_vm) && g_thumb_vm) {
        av_jni_set_java_vm(g_thumb_vm, NULL);
    }
    
    if (appctx) {
        g_thumb_appctx = env->NewGlobalRef(appctx);
        if (g_thumb_appctx) {
            av_jni_set_android_app_ctx(g_thumb_appctx, NULL);
        }
    }
}

// Clear codec cache and hardware context
jni_func(void, clearThumbnailCache) {
    {
        std::lock_guard<std::mutex> lock(g_codec_cache_mutex);
        g_codec_cache.clear();
    }
    
    {
        std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
        if (g_hw_device_ctx) {
            av_buffer_unref(&g_hw_device_ctx);
            g_hw_device_ctx = nullptr;
        }
        g_hw_ctx_initialized = false;
        g_hw_ctx_available = false;
    }
}

// Fast extraction is the only mode - optimized for speed

// Convert AVFrame to Android Bitmap
static jobject frame_to_bitmap(JNIEnv *env, AVFrame *frame, int target_dimension) {
    init_methods_cache(env);
    
    // Calculate scaled dimensions while preserving aspect ratio
    int width = frame->width;
    int height = frame->height;
    
    if (width > 0 && height > 0) {
        float scale = 1.0f;
        if (width >= height) {
            if (width > target_dimension) {
                scale = (float)target_dimension / width;
            }
        } else {
            if (height > target_dimension) {
                scale = (float)target_dimension / height;
            }
        }
        
        width = (int)(width * scale);
        height = (int)(height * scale);
    }
    
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    // Use fast bilinear scaling for speed
    int sws_algorithm = SWS_FAST_BILINEAR;

    // Create SwsContext for scaling and format conversion
    // Android Bitmap.Config.ARGB_8888 expects BGRA byte order (little-endian)
    struct SwsContext *sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        width, height, AV_PIX_FMT_BGRA,
        sws_algorithm, NULL, NULL, NULL
    );
    
    if (!sws_ctx) {
        ALOGE("Thumbnail | Failed to create scaler");
        return NULL;
    }
    
    jintArray arr = env->NewIntArray(width * height);
    if (!arr) {
        ALOGE("Thumbnail | Failed to allocate array");
        sws_freeContext(sws_ctx);
        return NULL;
    }
    
    jint *pixels = env->GetIntArrayElements(arr, NULL);
    if (!pixels) {
        ALOGE("Thumbnail | Failed to get array elements");
        env->DeleteLocalRef(arr);
        sws_freeContext(sws_ctx);
        return NULL;
    }
    
    uint8_t *dst_data[4] = { (uint8_t*)pixels };
    int dst_linesize[4] = { width * 4 };
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
    sws_freeContext(sws_ctx);
    env->ReleaseIntArrayElements(arr, pixels, 0);
    
    jobject bitmap_config = env->GetStaticObjectField(
        android_graphics_Bitmap_Config, 
        android_graphics_Bitmap_Config_ARGB_8888
    );
    
    if (!bitmap_config) {
        ALOGE("Thumbnail | Failed to get bitmap config");
        env->DeleteLocalRef(arr);
        return NULL;
    }
    
    jobject bitmap = env->CallStaticObjectMethod(
        android_graphics_Bitmap, 
        android_graphics_Bitmap_createBitmap,
        arr, width, height, bitmap_config
    );
    
    if (env->ExceptionCheck()) {
        ALOGE("Thumbnail | Exception creating bitmap");
        env->ExceptionClear();
        env->DeleteLocalRef(arr);
        env->DeleteLocalRef(bitmap_config);
        return NULL;
    }
    
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(bitmap_config);
    
    return bitmap;
}

jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension, jboolean use_hw_dec) {
    auto total_start = std::chrono::high_resolution_clock::now();
    
    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    init_methods_cache(env);
    
    // Validate parameters
    if (dimension <= 0 || dimension > 4096) {
        ALOGE("Thumbnail | Invalid dimension");
        return NULL;
    }
    
    if (position < 0.0) {
        ALOGE("Thumbnail | Invalid position");
        return NULL;
    }
    
    const char *path = env->GetStringUTFChars(jpath, NULL);
    if (!path) {
        ALOGE("Thumbnail | Invalid path");
        return NULL;
    }
    
    // Open video file
    AVFormatContext *format_ctx = NULL;
    if (avformat_open_input(&format_ctx, path, NULL, NULL) < 0) {
        ALOGE("Thumbnail | Failed to open file");
        env->ReleaseStringUTFChars(jpath, path);
        return NULL;
    }
    env->ReleaseStringUTFChars(jpath, path);
    
    // Find stream information (ultra-fast minimal analysis)
    format_ctx->max_analyze_duration = 100000;
    format_ctx->probesize = 500000;
    format_ctx->fps_probe_size = 1;
    format_ctx->max_ts_probe = 1;
    
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        ALOGE("Thumbnail | Failed to find stream info");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // Find video stream
    int video_stream_idx = -1;
    AVCodecParameters *codec_params = NULL;
    
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            codec_params = format_ctx->streams[i]->codecpar;
            break;
        }
    }
    
    if (video_stream_idx == -1) {
        ALOGE("Thumbnail | No video stream found");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVStream *video_stream = format_ctx->streams[video_stream_idx];
    
    // Initialize codec
    const AVCodec *codec = get_cached_codec(codec_params->codec_id);
    if (!codec) {
        ALOGE("Thumbnail | Codec not found");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        ALOGE("Thumbnail | Failed to allocate codec context");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        ALOGE("Thumbnail | Failed to copy codec params");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // Optimized for speed
    codec_ctx->thread_count = 0;
    codec_ctx->thread_type = FF_THREAD_SLICE;
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    codec_ctx->skip_frame = AVDISCARD_NONREF;
    codec_ctx->skip_idct = AVDISCARD_BIDIR;
    codec_ctx->skip_loop_filter = AVDISCARD_ALL;
    codec_ctx->export_side_data = 0;
    codec_ctx->err_recognition = 0;
    codec_ctx->workaround_bugs = 0;
    codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    
    // Enable hardware decoding if requested
    if (use_hw_dec && init_hw_device_context()) {
        std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
        if (g_hw_device_ctx) {
            codec_ctx->hw_device_ctx = av_buffer_ref(g_hw_device_ctx);
        }
    }
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        ALOGE("Thumbnail | Failed to open codec");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // Seek to position (skip if near start)
    if (position > 1.0 && position < INT64_MAX / AV_TIME_BASE) {
        int64_t timestamp = (int64_t)(position * AV_TIME_BASE);
        if (av_seek_frame(format_ctx, video_stream_idx, 
                          timestamp * video_stream->time_base.den / video_stream->time_base.num / AV_TIME_BASE,
                          AVSEEK_FLAG_ANY) < 0) {
            ALOGW("Thumbnail | Seek failed, using first frame");
        }
        avcodec_flush_buffers(codec_ctx);
    }
    
    // Decode frame
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!packet || !frame) {
        ALOGE("Thumbnail | Failed to allocate packet/frame");
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVFrame *rgb_frame = NULL;
    jobject bitmap = NULL;
    
    bool frame_found = false;
    int frames_decoded = 0;
    int packets_read = 0;
    const int MAX_FRAMES = 100;  // Reduced safety limit for speed (was 300)
    
    while (av_read_frame(format_ctx, packet) >= 0 && frames_decoded < MAX_FRAMES) {
        packets_read++;
        
        if (packet->stream_index == video_stream_idx) {
            // Send packet to decoder
            if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                // Receive decoded frame
                while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    frames_decoded++;
                    
                    // Calculate frame timestamp
                    double frame_time = 0.0;
                    if (frame->pts != AV_NOPTS_VALUE) {
                        frame_time = frame->pts * av_q2d(video_stream->time_base);
                    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        frame_time = frame->best_effort_timestamp * av_q2d(video_stream->time_base);
                    }
                    
                    // ULTRA FAST: Accept first frame if within reasonable range
                    // For maximum speed, we accept very lenient matching
                    const double skip_tolerance = 5.0;   // Skip frames more than 5s before target
                    const double match_tolerance = 5.0;  // Accept frames within 5s of target
                    
                    if (position > 0.0 && frame_time < position - skip_tolerance) {
                        av_frame_unref(frame);
                        continue;
                    }
                    
                    // Accept frame if close to target
                    if (position == 0.0 || frame_time >= position - match_tolerance) {
                        bitmap = frame_to_bitmap(env, frame, dimension);
                        if (bitmap) {
                            frame_found = true;
                        } else {
                            ALOGE("Thumbnail | Failed to convert frame");
                        }
                        break;
                    }
                    
                    av_frame_unref(frame);
                }
            }
            
            if (frame_found) {
                av_packet_unref(packet);
                break;
            }
        }
        
        av_packet_unref(packet);
    }
    
    // Cleanup
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    
    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start);
    
    if (!frame_found) {
        ALOGE("Thumbnail | Failed: no frame found");
        return NULL;
    }
    
    ALOGI("Thumbnail | %lldms", (long long)total_duration.count());
    return bitmap;
}

// ============================================================================
// ATTACHED PICTURE EXTRACTION (cover art / embedded thumbnail)
// Two passes:
//   1. Streams with AV_DISPOSITION_ATTACHED_PIC — MP4 covr, MP3 APIC,
//      and MKV cover art that FFmpeg auto-recognizes as a synthetic
//      single-frame video stream.
//   2. AVMEDIA_TYPE_ATTACHMENT streams with an image/* mimetype — the
//      genuine Matroska attachment case produced by
//      `yt-dlp --embed-thumbnail --merge-output-format mkv`.
// No seeking, no frame decoding loop — just decode the one attached packet.
// ============================================================================

static jobject decode_attached_picture(
    JNIEnv *env,
    const uint8_t *data,
    int size,
    AVCodecID codec_id,
    int dimension
) {
    const AVCodec *codec = get_cached_codec(codec_id);
    if (!codec) {
        ALOGD("AttachedPic | No decoder for codec_id=%d", (int)codec_id);
        return NULL;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) return NULL;

    codec_ctx->thread_count = 1;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        return NULL;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    jobject bm = NULL;

    if (pkt && frame) {
        // Wrap the caller's buffer without copying; the decoder only reads it.
        pkt->data = const_cast<uint8_t*>(data);
        pkt->size = size;

        if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
            // Signal EOF so single-frame image decoders (mjpeg/png/…) flush.
            avcodec_send_packet(codec_ctx, NULL);
            if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                bm = frame_to_bitmap(env, frame, dimension);
            }
        }
    }

    if (frame) av_frame_free(&frame);
    if (pkt) {
        pkt->data = NULL;
        pkt->size = 0;
        av_packet_free(&pkt);
    }
    avcodec_free_context(&codec_ctx);
    return bm;
}

jni_func(jobject, grabAttachedPicture, jstring jpath, jint dimension) {
    auto total_start = std::chrono::high_resolution_clock::now();

    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    init_methods_cache(env);

    if (dimension <= 0 || dimension > 4096) {
        ALOGE("AttachedPic | Invalid dimension");
        return NULL;
    }

    const char *path = env->GetStringUTFChars(jpath, NULL);
    if (!path) {
        ALOGE("AttachedPic | Invalid path");
        return NULL;
    }

    AVFormatContext *format_ctx = NULL;
    if (avformat_open_input(&format_ctx, path, NULL, NULL) < 0) {
        ALOGE("AttachedPic | Failed to open file");
        env->ReleaseStringUTFChars(jpath, path);
        return NULL;
    }
    env->ReleaseStringUTFChars(jpath, path);

    // Minimal probe — same numbers as grabThumbnailFast
    format_ctx->max_analyze_duration = 100000;
    format_ctx->probesize = 500000;

    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        ALOGE("AttachedPic | Failed to find stream info");
        avformat_close_input(&format_ctx);
        return NULL;
    }

    jobject bitmap = NULL;

    // Diagnostic: dump every stream so we can tell what a container actually
    // carries when a real-world file doesn't match our expectations.
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        AVStream *stream = format_ctx->streams[i];
        AVCodecParameters *par = stream->codecpar;
        AVDictionaryEntry *mime = av_dict_get(stream->metadata, "mimetype", NULL, 0);
        AVDictionaryEntry *fname = av_dict_get(stream->metadata, "filename", NULL, 0);
        ALOGD("AttachedPic | stream[%u] type=%d codec=%s disp=0x%x extradata=%d attached_pic=%d mime=%s filename=%s",
              i, (int)par->codec_type,
              avcodec_get_name(par->codec_id),
              stream->disposition,
              par->extradata_size,
              stream->attached_pic.size,
              (mime && mime->value) ? mime->value : "-",
              (fname && fname->value) ? fname->value : "-");
    }

    // Ordered list of image decoders to fall back through when the stream's
    // codec_id is unknown or its declared codec fails to decode.
    static const AVCodecID FALLBACK_IMAGE_CODECS[] = {
        AV_CODEC_ID_MJPEG,
        AV_CODEC_ID_PNG,
        AV_CODEC_ID_WEBP,
        AV_CODEC_ID_BMP,
        AV_CODEC_ID_GIF,
    };
    constexpr int FALLBACK_COUNT =
        sizeof(FALLBACK_IMAGE_CODECS) / sizeof(FALLBACK_IMAGE_CODECS[0]);

    auto try_decode_all = [&](const uint8_t *data, int size, AVCodecID declared) -> jobject {
        if (!data || size <= 0) return NULL;
        // Try the declared codec first if it's plausible.
        if (declared != AV_CODEC_ID_NONE) {
            jobject bm = decode_attached_picture(env, data, size, declared, dimension);
            if (bm) return bm;
        }
        // Then walk the fallback list, skipping the one we already tried.
        for (int i = 0; i < FALLBACK_COUNT; i++) {
            if (FALLBACK_IMAGE_CODECS[i] == declared) continue;
            jobject bm = decode_attached_picture(
                env, data, size, FALLBACK_IMAGE_CODECS[i], dimension
            );
            if (bm) return bm;
        }
        return NULL;
    };

    // Pass 1: AV_DISPOSITION_ATTACHED_PIC — canonical cover-art case.
    // Covers MP4 covr, MP3 APIC, Ogg/Opus METADATA_BLOCK_PICTURE (FFmpeg
    // auto-lifts it into a synthetic video stream), and any container where
    // yt-dlp attached the picture via `-disposition:v:N attached_pic`.
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        AVStream *stream = format_ctx->streams[i];
        if (!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;

        AVPacket *pic = &stream->attached_pic;
        if (!pic->data || pic->size <= 0) continue;

        bitmap = try_decode_all(pic->data, pic->size, stream->codecpar->codec_id);
        if (bitmap) {
            ALOGI("AttachedPic | ATTACHED_PIC hit stream[%u] codec=%s",
                  i, avcodec_get_name(stream->codecpar->codec_id));
            break;
        }
    }

    // Pass 2: AVMEDIA_TYPE_ATTACHMENT — Matroska/WebM attachments (`-attach`).
    // Take any attachment with extradata and try to decode it as an image.
    // WebM attachments produced by yt-dlp sometimes come through without a
    // mimetype tag and with codec_id=NONE, so a strict image-only filter
    // (like before) would drop them. Better to decode-attempt and let the
    // decoder tell us whether the bytes are a picture.
    if (!bitmap) {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVStream *stream = format_ctx->streams[i];
            AVCodecParameters *par = stream->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_ATTACHMENT) continue;
            if (!par->extradata || par->extradata_size <= 0) continue;

            bitmap = try_decode_all(par->extradata, par->extradata_size, par->codec_id);
            if (bitmap) {
                AVDictionaryEntry *mime = av_dict_get(stream->metadata, "mimetype", NULL, 0);
                ALOGI("AttachedPic | ATTACHMENT hit stream[%u] codec=%s mime=%s",
                      i, avcodec_get_name(par->codec_id),
                      (mime && mime->value) ? mime->value : "-");
                break;
            }
        }
    }

    // Pass 3: raw magic-byte sniff on any stream's extradata. Defensive
    // catch-all for containers where FFmpeg preserved the raw JPEG/PNG/WebP
    // bytes on some non-attachment stream (rare, but zero-cost to check).
    if (!bitmap) {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVCodecParameters *par = format_ctx->streams[i]->codecpar;
            if (!par->extradata || par->extradata_size < 8) continue;
            if (par->codec_type == AVMEDIA_TYPE_ATTACHMENT) continue; // handled in Pass 2

            const uint8_t *d = par->extradata;
            bool looks_image =
                (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) ||                       // JPEG
                (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') ||          // PNG
                (d[0] == 'R'  && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
                 d[8] == 'W' && d[9] == 'E' && d[10]== 'B' && d[11]== 'P') ||           // WebP
                (d[0] == 'B'  && d[1] == 'M') ||                                        // BMP
                (d[0] == 'G'  && d[1] == 'I' && d[2] == 'F' && d[3] == '8');            // GIF
            if (!looks_image) continue;

            bitmap = try_decode_all(par->extradata, par->extradata_size, par->codec_id);
            if (bitmap) {
                ALOGI("AttachedPic | MAGIC hit stream[%u] type=%d codec=%s",
                      i, (int)par->codec_type, avcodec_get_name(par->codec_id));
                break;
            }
        }
    }

    avformat_close_input(&format_ctx);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start);

    if (bitmap) {
        ALOGI("AttachedPic | %lldms", (long long)total_duration.count());
    } else {
        ALOGD("AttachedPic | none (%lldms)", (long long)total_duration.count());
    }

    return bitmap;
}
