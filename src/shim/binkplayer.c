// ARM64 replacement for PZ's libbink64.so (VideoTexture JNI). The game's videos
// are Bink 1, which ffmpeg decodes. Frames are delivered as RGBA at the video's
// native size, paced to the video's frame rate, and loop at the end like Bink.
#include <jni.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define MAXV 64
typedef struct {
    AVFormatContext *fmt; AVCodecContext *dec; struct SwsContext *sws;
    AVPacket *pkt; AVFrame *frm; int stream;
    uint8_t *rgba; int w, h;
    double dur, due;   // seconds per frame, wall time the next frame is due
    int64_t shown;
} Video;

static Video *vids[MAXV];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static Video *get(jint id) { return (id >= 0 && id < MAXV) ? vids[id] : NULL; }

static void vfree(Video *v) {
    if (!v) return;
    sws_freeContext(v->sws); av_frame_free(&v->frm); av_packet_free(&v->pkt);
    avcodec_free_context(&v->dec); avformat_close_input(&v->fmt); free(v->rgba); free(v);
}

// Decode the next frame into v->rgba, looping to the start at end of stream.
static int decode_next(Video *v) {
    for (int restarts = 0; restarts < 2;) {
        int r = avcodec_receive_frame(v->dec, v->frm);
        if (r == 0) {
            uint8_t *dst[4] = { v->rgba }; int ls[4] = { v->w * 4 };
            sws_scale(v->sws, (const uint8_t *const *)v->frm->data, v->frm->linesize, 0, v->h, dst, ls);
            return 1;
        }
        if (r == AVERROR_EOF) {
            av_seek_frame(v->fmt, v->stream, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(v->dec); restarts++; continue;
        }
        r = av_read_frame(v->fmt, v->pkt);
        if (r < 0) { avcodec_send_packet(v->dec, NULL); continue; }  // drain
        if (v->pkt->stream_index == v->stream) avcodec_send_packet(v->dec, v->pkt);
        av_packet_unref(v->pkt);
    }
    return 0;
}

#define J(ret, name, ...) JNIEXPORT ret JNICALL Java_zombie_core_textures_VideoTexture_##name(JNIEnv *env, jobject self, ##__VA_ARGS__)

J(jint, openVideo, jstring jpath) {
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    Video *v = calloc(1, sizeof *v); int id = -1;
    if (avformat_open_input(&v->fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(v->fmt, NULL) < 0) goto fail;
    const AVCodec *c = NULL;
    v->stream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &c, 0);
    if (v->stream < 0 || !c) goto fail;
    AVStream *st = v->fmt->streams[v->stream];
    v->dec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(v->dec, st->codecpar);
    if (avcodec_open2(v->dec, c, NULL) < 0) goto fail;
    v->w = v->dec->width; v->h = v->dec->height;
    v->sws = sws_getContext(v->w, v->h, v->dec->pix_fmt, v->w, v->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    v->rgba = calloc((size_t)v->w * v->h, 4);
    v->pkt = av_packet_alloc(); v->frm = av_frame_alloc();
    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    v->dur = fr.num ? (double)fr.den / fr.num : 1.0 / 30;
    if (!v->sws || !v->rgba) goto fail;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAXV; i++) if (!vids[i]) { vids[i] = v; id = i; break; }
    pthread_mutex_unlock(&lock);
    if (id < 0) goto fail;
    v->due = now();
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return id;
fail:
    vfree(v);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return -1;
}

J(jboolean, isReadyForNewFrame, jint id) { Video *v = get(id); return v && now() >= v->due; }
J(void, processFrame, jint id) { Video *v = get(id); if (v) decode_next(v); }
J(void, processFrameAsync, jint id) { Video *v = get(id); if (v) decode_next(v); }
J(jboolean, processFrameAsyncWait, jint id, jint timeoutMs) { return get(id) != NULL; }
J(void, nextFrame, jint id) {
    Video *v = get(id); if (!v) return;
    v->shown++; v->due += v->dur;
    if (now() - v->due > 1.0) v->due = now();  // don't try to catch up after a long stall
}
J(jboolean, shouldSkipFrame, jint id) { Video *v = get(id); return v && now() > v->due + v->dur; }
J(jboolean, isEndOfVideo, jint id) { return JNI_FALSE; }  // loops forever, like Bink
J(void, closeVideo, jint id) {
    pthread_mutex_lock(&lock);
    Video *v = get(id); if (v) vids[id] = NULL;
    pthread_mutex_unlock(&lock);
    vfree(v);
}
J(jlong, getCurrentFrameData, jint id) { Video *v = get(id); return v ? (jlong)(intptr_t)v->rgba : 0; }
J(jint, getFrameDataSize, jint id) { Video *v = get(id); return v ? v->w * v->h * 4 : 0; }
