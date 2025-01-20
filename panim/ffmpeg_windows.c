#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <unistd.h>

#include <raylib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>


#define STREAM_DURATION   30.0
#define STREAM_FRAME_RATE 30 /* 30 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC
typedef struct OutputStream{
    AVStream* st;
    AVCodecContext* enc;

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;

    AVFrame* frame;
    AVFrame* tmp_frame;

    AVPacket* tmp_pkt;

    float t, tincr, tincr2;

    struct SwsContext* sws_ctx;
    struct SwrContext* swr_ctx;
} OutputStream;

typedef struct FFMPEG {
    OutputStream video_st, audio_st;
    const AVOutputFormat* fmt;
    const char* filename;
    AVFormatContext* oc;
    const AVCodec* audio_codec, * video_codec;
    int ret;
    int have_video, have_audio;
    int encode_video, encode_audio;
    AVDictionary* opt;
    int width, height;
    int fps;
} FFMPEG;

static void log_packet(const AVFormatContext* fmt_ctx, const AVPacket* pkt){
    AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    char buf4[AV_TS_MAX_STRING_SIZE];
    char buf5[AV_TS_MAX_STRING_SIZE];
    av_ts_make_time_string(buf4, pkt->pts, time_base);
    av_ts_make_time_string(buf5, pkt->dts, time_base);
    printf("pts_time:%s/%f dts_time:%s stream_index:%d\n",
        buf4, STREAM_DURATION,
        buf5,
        pkt->stream_index);
}

static int write_frame(AVFormatContext* fmt_ctx, AVCodecContext* c,
    AVStream* st, AVFrame* frame, AVPacket* pkt){
    int ret;

    // send the frame to the encoder
    ret = avcodec_send_frame(c, frame);
    if(ret < 0){
        fprintf(stderr, "Error sending a frame to the encoder: %s\n", av_err2str(ret));
        exit(1);
    }

    while(ret >= 0){
        ret = avcodec_receive_packet(c, pkt);
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if(ret < 0){
            fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
            exit(1);
        }

        /* rescale output packet timestamp values from codec to stream timebase */
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;

        /* Write the compressed frame to the media file. */
        log_packet(fmt_ctx, pkt);
        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if(ret < 0){
            fprintf(stderr, "Error while writing output packet: %s\n", av_err2str(ret));
            exit(1);
        }
    }

    return ret == AVERROR_EOF ? 1 : 0;
}

static void add_stream(OutputStream* ost, AVFormatContext* oc,
    const AVCodec** codec,
    enum AVCodecID codec_id,FFMPEG*ffmpeg){
    AVCodecContext* c;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if(!(*codec)){
        fprintf(stderr, "Could not find encoder for '%s'\n",
            avcodec_get_name(codec_id));
        exit(1);
    }

    ost->tmp_pkt = av_packet_alloc();
    if(!ost->tmp_pkt){
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if(!ost->st){
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams - 1;
    c = avcodec_alloc_context3(*codec);
    if(!c){
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;
    AVChannelLayout x = { /* .order */ AV_CHANNEL_ORDER_NATIVE, /* .nb_channels */  (2), /* .u.mask */ { AV_CH_LAYOUT_STEREO }, /* .opaque */ NULL};
    switch((*codec)->type){
    case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt = /*(*codec)->sample_fmts ?
            (*codec)->sample_fmts[0] :*/ AV_SAMPLE_FMT_FLTP;
        c->bit_rate = 64000;
        c->sample_rate = 44100;
        //avcodec_get_supported_config(c,*codec,)
        /*if((*codec)->supported_samplerates){
            c->sample_rate = (*codec)->supported_samplerates[0];
            for(int i = 0; (*codec)->supported_samplerates[i]; i++){
                if((*codec)->supported_samplerates[i] == 44100)
                    c->sample_rate = 44100;
            }
        }*/

        av_channel_layout_copy(&c->ch_layout, &x);
        ost->st->time_base = (AVRational){1, c->sample_rate};
        break;

    case AVMEDIA_TYPE_VIDEO:
        c->codec_id = codec_id;

        c->bit_rate = 4000000;
        /* Resolution must be a multiple of two. */
        c->width = ffmpeg->width;
        c->height = ffmpeg->height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){1, ffmpeg->fps};
        c->time_base = ost->st->time_base;

        c->gop_size = 20; /* emit one intra frame every twelve frames at most */
        c->pix_fmt = STREAM_PIX_FMT;
        if(c->codec_id == AV_CODEC_ID_MPEG2VIDEO){
            /* just for testing, we also add B-frames */
            c->max_b_frames = 2;
        }
        if(c->codec_id == AV_CODEC_ID_MPEG1VIDEO){
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;
        }
        break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if(oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

static AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt,
    const AVChannelLayout* channel_layout,
    int sample_rate, int nb_samples){
    AVFrame* frame = av_frame_alloc();
    if(!frame){
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }

    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if(nb_samples){
        if(av_frame_get_buffer(frame, 0) < 0){
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

static void open_audio(const AVCodec* codec,
    OutputStream* ost, AVDictionary* opt_arg){
    AVCodecContext* c;
    int nb_samples;
    int ret;
    AVDictionary* opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if(ret < 0){
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* init signal generator */
    ost->t = 0;
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

    if(c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;

    ost->frame = alloc_audio_frame(c->sample_fmt, &c->ch_layout,
        c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
        c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if(ret < 0){
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
    ost->swr_ctx = swr_alloc();
    if(!ost->swr_ctx){
        fprintf(stderr, "Could not allocate resampler context\n");
        exit(1);
    }

    /* set options */
    av_opt_set_chlayout(ost->swr_ctx, "in_chlayout", &c->ch_layout, 0);
    av_opt_set_int(ost->swr_ctx, "in_sample_rate", c->sample_rate, 0);
    av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    av_opt_set_chlayout(ost->swr_ctx, "out_chlayout", &c->ch_layout, 0);
    av_opt_set_int(ost->swr_ctx, "out_sample_rate", c->sample_rate, 0);
    av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

    /* initialize the resampling context */
    if((ret = swr_init(ost->swr_ctx)) < 0){
        fprintf(stderr, "Failed to initialize the resampling context\n");
        exit(1);
    }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
static AVFrame* get_audio_frame(OutputStream* ost){
    AVFrame* frame = ost->tmp_frame;
    int j, i, v;
    int16_t* q = (int16_t*) frame->data[0];
    return NULL;

    /* check if we want to generate more frames */
    if(av_compare_ts(ost->next_pts, ost->enc->time_base,
        STREAM_DURATION, (AVRational){1, 1}) > 0)
        return NULL;

    for(j = 0; j < frame->nb_samples; j++){
        v = (int) (sin(ost->t) * 10000);
        for(i = 0; i < ost->enc->ch_layout.nb_channels; i++)
            *q++ = v;
        ost->t += ost->tincr;
        ost->tincr += ost->tincr2;
    }

    frame->pts = ost->next_pts;
    ost->next_pts += frame->nb_samples;

    return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(AVFormatContext* oc, OutputStream* ost){
    AVCodecContext* c;
    AVFrame* frame;
    int ret;
    int dst_nb_samples;

    c = ost->enc;

    frame = get_audio_frame(ost);

    if(frame){
        /* convert samples from native format to destination codec format, using the resampler */
        /* compute destination number of samples */
        dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples,
            c->sample_rate, c->sample_rate, AV_ROUND_UP);
        av_assert0(dst_nb_samples == frame->nb_samples);

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally;
         * make sure we do not overwrite it here
         */
        ret = av_frame_make_writable(ost->frame);
        if(ret < 0)
            exit(1);

        /* convert to destination format */
        ret = swr_convert(ost->swr_ctx,
            ost->frame->data, dst_nb_samples,
            (const uint8_t**) frame->data, frame->nb_samples);
        if(ret < 0){
            fprintf(stderr, "Error while converting\n");
            exit(1);
        }
        frame = ost->frame;

        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
        ost->samples_count += dst_nb_samples;
    }

    return write_frame(oc, c, ost->st, frame, ost->tmp_pkt);
}

/**************************************************************/
/* video output */

static AVFrame* alloc_picture(enum AVPixelFormat pix_fmt, int width, int height){
    AVFrame* picture;
    int ret;

    picture = av_frame_alloc();
    if(!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 0);
    if(ret < 0){
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return picture;
}

static void open_video(const AVCodec* codec,
    OutputStream* ost, AVDictionary* opt_arg){
    int ret;
    AVCodecContext* c = ost->enc;
    AVDictionary* opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if(ret < 0){
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if(!ost->frame){
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if(c->pix_fmt != AV_PIX_FMT_BGR32){
        ost->tmp_frame = alloc_picture(AV_PIX_FMT_BGR32, c->width, c->height);
        if(!ost->tmp_frame){
            fprintf(stderr, "Could not allocate temporary picture\n");
            exit(1);
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if(ret < 0){
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

/* Prepare a dummy image. */
static void fill_image(AVFrame* pict, int frame_index,
    int width, int height, void* data){
    int x, y, i;

    i = frame_index;
    uint32_t* d = (uint32_t*) data;
    /* RGBA */
    for(y = 0; y < height; y++){
        for(x = 0; x < width; x++){
            int pos = (x + (height-y-1) * width);
            int pos2 = x * 4 + y * pict->linesize[0];
            pict->data[0][pos2 + 0] = d[pos] & 0xff;
            pict->data[0][pos2 + 1] = (d[pos] & 0xff00) >> 8;
            pict->data[0][pos2 + 2] = (d[pos] & 0xff0000) >> 16;
            pict->data[0][pos2 + 3] = (d[pos] & 0xff000000) >> 24;
        }
    }
}

static AVFrame* get_video_frame(OutputStream* ost, void* data){
    AVCodecContext* c = ost->enc;

    /* check if we want to generate more frames */
    if(av_compare_ts(ost->next_pts, c->time_base,
        STREAM_DURATION, (AVRational){1, 1}) > 0)
        return NULL;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if(av_frame_make_writable(ost->frame) < 0)
        exit(1);

    if(c->pix_fmt != AV_PIX_FMT_BGR32){
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if(!ost->sws_ctx){
            ost->sws_ctx = sws_getContext(c->width, c->height,
                AV_PIX_FMT_BGR32,
                c->width, c->height,
                c->pix_fmt,
                SCALE_FLAGS, NULL, NULL, NULL);
            if(!ost->sws_ctx){
                fprintf(stderr,
                    "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        fill_image(ost->tmp_frame, ost->next_pts, c->width, c->height, data);
        sws_scale(ost->sws_ctx, (const uint8_t* const*) ost->tmp_frame->data,
            ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
            ost->frame->linesize);
    } else{
        fill_image(ost->frame, ost->next_pts, c->width, c->height, data);
    }

    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext* oc, OutputStream* ost,void* data){
    return write_frame(oc, ost->enc, ost->st, get_video_frame(ost,data), ost->tmp_pkt);
}

static void close_stream(OutputStream* ost){
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

FFMPEG *ffmpeg_start_rendering_video(const char *filename, size_t width, size_t height, size_t fps)
{
    OutputStream video_st = {0}, audio_st = {0};
    const AVOutputFormat* fmt;
    AVFormatContext* oc;
    const AVCodec* audio_codec = NULL, * video_codec = NULL;
    int ret;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;
    AVDictionary* opt = NULL;
    FFMPEG* ffmpeg = malloc(sizeof(FFMPEG));
    ffmpeg->width = width;
    ffmpeg->height = height;
    ffmpeg->fps = fps;
    /* allocate the output media context */
    avformat_alloc_output_context2(&oc, NULL, NULL, filename);
    if(!oc){
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
    }
    if(!oc)
        return NULL;

    fmt = oc->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if(fmt->video_codec != AV_CODEC_ID_NONE){
        add_stream(&video_st, oc, &video_codec, fmt->video_codec, ffmpeg);
        have_video = 1;
        encode_video = 1;
    }
    if(fmt->audio_codec != AV_CODEC_ID_NONE){
        add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec, ffmpeg);
        have_audio = 1;
        encode_audio = 1;
    }

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if(have_video)
        open_video(video_codec, &video_st, opt);

    if(have_audio)
        open_audio(audio_codec, &audio_st, opt);

    av_dump_format(oc, 0, filename, 1);

    /* open the output file, if needed */
    if(!(fmt->flags & AVFMT_NOFILE)){
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
        if(ret < 0){
            fprintf(stderr, "Could not open '%s': %s\n", filename,
                av_err2str(ret));
            return NULL;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(oc, &opt);
    if(ret < 0){
        fprintf(stderr, "Error occurred when opening output file: %s\n",
            av_err2str(ret));
        return NULL;
    }
    ffmpeg->audio_codec = audio_codec;
    ffmpeg->audio_st = audio_st;
    ffmpeg->encode_audio = encode_audio;
    ffmpeg->encode_video = encode_video;
    ffmpeg->filename = filename;
    ffmpeg->fmt = fmt;
    ffmpeg->have_audio = have_audio;
    ffmpeg->have_video = have_video;
    ffmpeg->oc = oc;
    ffmpeg->opt = opt;
    ffmpeg->video_codec = video_codec;
    ffmpeg->video_st = video_st;
    ffmpeg->ret = ret;
    return ffmpeg;
}

FFMPEG *ffmpeg_start_rendering_audio(const char *output_path)
{
    (void) output_path;
    return NULL;
}

bool ffmpeg_end_rendering(FFMPEG *ffmpeg, bool cancel)
{
    (void) cancel;
    av_write_trailer(ffmpeg->oc);

    /* Close each codec. */
    if(ffmpeg->have_video)
        close_stream(&ffmpeg->video_st);
    if(ffmpeg->have_audio)
        close_stream(&ffmpeg->audio_st);

    if(!(ffmpeg->fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_closep(&ffmpeg->oc->pb);

    /* free the stream */
    avformat_free_context(ffmpeg->oc);
    free(ffmpeg);
    printf("render_end\n");
    return true;
}

bool ffmpeg_send_frame_flipped(FFMPEG *ffmpeg, void *data, size_t width, size_t height)
{
    (void) width;
    (void) height;
    ffmpeg->encode_video = !write_video_frame(ffmpeg->oc, &ffmpeg->video_st,data);
    return ffmpeg->encode_video;
}

bool ffmpeg_send_sound_samples(FFMPEG *ffmpeg, void *data, size_t size)
{
    (void) ffmpeg;
    (void) data;
    (void) size;
    return true;
}
