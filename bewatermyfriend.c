#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <ncurses.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>

const char *ASCII_CHARS = " .:-=+*#%@";

typedef struct {
    AVFormatContext *format_ctx;
    AVCodecContext *video_codec_ctx;
    AVCodecContext *audio_codec_ctx;
    int video_stream_index;
    int audio_stream_index;
    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;
    AVPacket *pkt;
    AVFrame *frame;
    AVFrame *rgb_frame;

    // audio output & buffers (SDL)
    SDL_AudioDeviceID audio_dev;
    int out_sample_rate;
    enum AVSampleFormat out_sample_fmt;
    int out_channels;

    uint8_t **audio_dst_data;
    int audio_dst_linesize;
    int max_dst_nb_samples;

    int quit;
} PlayerState;

// convert a frame to ASCII art
static void frame_to_ascii(AVFrame *frame, int width, int height) {
    int console_width, console_height;
    getmaxyx(stdscr, console_height, console_width);

    float scale_x = (float)width / console_width;
    float scale_y = (float)height / console_height;

    for (int y = 0; y < console_height; y++) {
        for (int x = 0; x < console_width; x++) {
            int frame_x = (int)(x * scale_x);
            int frame_y = (int)(y * scale_y);
            if (frame_x >= width) frame_x = width - 1;
            if (frame_y >= height) frame_y = height - 1;

            int idx = frame_y * frame->linesize[0] + frame_x * 3;
            uint8_t r = frame->data[0][idx];
            uint8_t g = frame->data[0][idx + 1];
            uint8_t b = frame->data[0][idx + 2];
            int brightness = (r + g + b) / 3;
            int char_idx = brightness * ((int)strlen(ASCII_CHARS) - 1) / 255;
            mvaddch(y, x, ASCII_CHARS[char_idx]);
        }
    }
    refresh();
}

// initialize the player
static int init_player(PlayerState *state, const char *filename) {
    if (avformat_open_input(&state->format_ctx, filename, NULL, NULL) != 0) {
        fprintf(stderr, "Could not open file %s\n", filename);
        return -1;
    }

    if (avformat_find_stream_info(state->format_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        return -1;
    }

    state->video_stream_index = -1;
    state->audio_stream_index = -1;

    for (unsigned i = 0; i < state->format_ctx->nb_streams; i++) {
        enum AVMediaType t = state->format_ctx->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && state->video_stream_index == -1) {
            state->video_stream_index = (int)i;
        } else if (t == AVMEDIA_TYPE_AUDIO && state->audio_stream_index == -1) {
            state->audio_stream_index = (int)i;
        }
    }

    if (state->video_stream_index == -1) {
        fprintf(stderr, "Could not find video stream\n");
        return -1;
    }

    AVCodecParameters *video_codec_params = state->format_ctx->streams[state->video_stream_index]->codecpar;
    const AVCodec *video_codec = avcodec_find_decoder(video_codec_params->codec_id);
    if (!video_codec) {
        fprintf(stderr, "Unsupported video codec\n");
        return -1;
    }

    state->video_codec_ctx = avcodec_alloc_context3(video_codec);
    avcodec_parameters_to_context(state->video_codec_ctx, video_codec_params);

    if (avcodec_open2(state->video_codec_ctx, video_codec, NULL) < 0) {
        fprintf(stderr, "Could not open video codec\n");
        return -1;
    }

    state->audio_dev = 0;
    state->audio_dst_data = NULL;
    state->audio_dst_linesize = 0;
    state->max_dst_nb_samples = 0;
    state->out_sample_rate = 44100;
    state->out_sample_fmt = AV_SAMPLE_FMT_S16;
    state->out_channels = 2;

    if (state->audio_stream_index != -1) {
        AVCodecParameters *audio_codec_params = state->format_ctx->streams[state->audio_stream_index]->codecpar;
        const AVCodec *audio_codec = avcodec_find_decoder(audio_codec_params->codec_id);
        if (!audio_codec) {
            fprintf(stderr, "Unsupported audio codec\n");
            return -1;
        }

        state->audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        avcodec_parameters_to_context(state->audio_codec_ctx, audio_codec_params);

        if (avcodec_open2(state->audio_codec_ctx, audio_codec, NULL) < 0) {
            fprintf(stderr, "Could not open audio codec\n");
            return -1;
        }

        // initialize audio resampler to S16/stereo/44.1k
        state->swr_ctx = swr_alloc();
        if (!state->swr_ctx) {
            fprintf(stderr, "swr_alloc failed\n");
            return -1;
        }

        av_opt_set_chlayout(state->swr_ctx, "in_chlayout",  &state->audio_codec_ctx->ch_layout, 0);
        AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
        av_opt_set_chlayout(state->swr_ctx, "out_chlayout", &out_ch, 0);
        av_opt_set_int(state->swr_ctx, "in_sample_rate",  state->audio_codec_ctx->sample_rate, 0);
        av_opt_set_int(state->swr_ctx, "out_sample_rate", state->out_sample_rate, 0);
        av_opt_set_sample_fmt(state->swr_ctx, "in_sample_fmt",  state->audio_codec_ctx->sample_fmt, 0);
        av_opt_set_sample_fmt(state->swr_ctx, "out_sample_fmt", state->out_sample_fmt, 0);

        if (swr_init(state->swr_ctx) < 0) {
            fprintf(stderr, "swr_init failed\n");
            return -1;
        }

        // SDL2 audio device (queue API, no callback)
        if (SDL_Init(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
            return -1;
        }

        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = state->out_sample_rate;
        want.format = AUDIO_S16SYS;
        want.channels = state->out_channels;
        want.samples = 2048; 
        want.callback = NULL; 

        state->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (state->audio_dev == 0) {
            fprintf(stderr, "SDL_OpenAudioDevice error: %s\n", SDL_GetError());
            return -1;
        }

        state->out_sample_rate = have.freq;
        state->out_channels    = have.channels;
        SDL_PauseAudioDevice(state->audio_dev, 0);

        int64_t dst_ch_layout = AV_CH_LAYOUT_STEREO;
        int dst_nb_channels = out_ch.nb_channels;
        state->max_dst_nb_samples = 0;
        int ret = av_samples_alloc_array_and_samples(
            &state->audio_dst_data,
            &state->audio_dst_linesize,
            dst_nb_channels,
            1024,                          
            state->out_sample_fmt,
            1
        );

        if (ret < 0) {
            fprintf(stderr, "av_samples_alloc_array_and_samples failed\n");
            return -1;
        }
    }

    // initialize scaling context for converting frames to RGB
    state->sws_ctx = sws_getContext(
        state->video_codec_ctx->width,
        state->video_codec_ctx->height,
        state->video_codec_ctx->pix_fmt,
        state->video_codec_ctx->width,
        state->video_codec_ctx->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        NULL, NULL, NULL
    );

    // allocate frames and packet
    state->frame = av_frame_alloc();
    state->rgb_frame = av_frame_alloc();
    state->pkt = av_packet_alloc();

    // allocate RGB buffer
    int num_bytes = av_image_get_buffer_size(
        AV_PIX_FMT_RGB24,
        state->video_codec_ctx->width,
        state->video_codec_ctx->height, 1
    );
    uint8_t *buffer = (uint8_t *)av_malloc(num_bytes * sizeof(uint8_t));
    av_image_fill_arrays(state->rgb_frame->data, state->rgb_frame->linesize, buffer,
                         AV_PIX_FMT_RGB24,
                         state->video_codec_ctx->width,
                         state->video_codec_ctx->height, 1);

    state->quit = 0;
    return 0;
}

// clean up resources
static void cleanup_player(PlayerState *state) {
    if (state->video_codec_ctx) avcodec_free_context(&state->video_codec_ctx);
    if (state->audio_codec_ctx) avcodec_free_context(&state->audio_codec_ctx);
    if (state->format_ctx) avformat_close_input(&state->format_ctx);
    if (state->sws_ctx) sws_freeContext(state->sws_ctx);
    if (state->swr_ctx) swr_free(&state->swr_ctx);
    if (state->frame) av_frame_free(&state->frame);
    if (state->rgb_frame) {
        if (state->rgb_frame->data[0]) av_free(state->rgb_frame->data[0]);
        av_frame_free(&state->rgb_frame);
    }

    if (state->pkt) av_packet_free(&state->pkt);

    if (state->audio_dst_data) {
        av_freep(&state->audio_dst_data[0]);
        av_freep(&state->audio_dst_data);
    }

    if (state->audio_dev) {
        SDL_CloseAudioDevice(state->audio_dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_Quit();
    }
}

// helper: queue decoded+resampled audio frame to SDL
static void queue_audio(PlayerState *state, AVFrame *in) {
    if (!state->audio_dev) return;

    int64_t delay = swr_get_delay(state->swr_ctx, state->audio_codec_ctx->sample_rate);
    int64_t dst_nb_samples = av_rescale_rnd(
        delay + in->nb_samples,
        state->out_sample_rate,
        state->audio_codec_ctx->sample_rate,
        AV_ROUND_UP
    );

    int dst_nb_channels = state->out_channels;

    if (dst_nb_samples > state->max_dst_nb_samples) {
        state->max_dst_nb_samples = (int)dst_nb_samples;
        av_freep(&state->audio_dst_data[0]);
        av_samples_alloc(
            &state->audio_dst_data[0],
            &state->audio_dst_linesize,
            dst_nb_channels,
            state->max_dst_nb_samples,
            state->out_sample_fmt,
            1
        );
    }

    // convert
    int converted = swr_convert(
        state->swr_ctx,
        &state->audio_dst_data[0],
        (int)dst_nb_samples,
        (const uint8_t **)in->data,
        in->nb_samples
    );

    if (converted < 0) {
        return;
    }

    int bytes_per_sample = av_get_bytes_per_sample(state->out_sample_fmt);
    int data_size = converted * dst_nb_channels * bytes_per_sample;

    // queue to SDL
    if (SDL_QueueAudio(state->audio_dev, state->audio_dst_data[0], (Uint32)data_size) != 0) {
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <video_file>\n", argv[0]);
        return 1;
    }

    PlayerState state;
    memset(&state, 0, sizeof(state));

    if (init_player(&state, argv[1]) < 0) {
        fprintf(stderr, "Failed to initialize player\n");
        cleanup_player(&state);
        return 1;
    }

    // initialize NCurses
    initscr();
    curs_set(0);
    timeout(0);

    // main demux/decode loop
    while (!state.quit) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            state.quit = 1;
            break;
        }

        if (av_read_frame(state.format_ctx, state.pkt) >= 0) {
            if (state.pkt->stream_index == state.video_stream_index) {
                if (avcodec_send_packet(state.video_codec_ctx, state.pkt) == 0) {
                    while (avcodec_receive_frame(state.video_codec_ctx, state.frame) == 0) {
                        sws_scale(state.sws_ctx,
                                  (const uint8_t *const *)state.frame->data,
                                  state.frame->linesize,
                                  0,
                                  state.video_codec_ctx->height,
                                  state.rgb_frame->data,
                                  state.rgb_frame->linesize);

                        frame_to_ascii(state.rgb_frame,
                                       state.video_codec_ctx->width,
                                       state.video_codec_ctx->height);

                        // frame pacing using avg_frame_rate
                        AVRational fr = state.format_ctx->streams[state.video_stream_index]->avg_frame_rate;
                        double fps = (fr.num && fr.den) ? av_q2d(fr) : 30.0;
                        if (fps <= 0.0) fps = 30.0;
                        useconds_t delay = (useconds_t)(1000000.0 / fps);
                        usleep(delay);
                    }
                }
            } else if (state.pkt->stream_index == state.audio_stream_index && state.audio_codec_ctx) {
                if (avcodec_send_packet(state.audio_codec_ctx, state.pkt) == 0) {
                    while (1) {
                        int r = avcodec_receive_frame(state.audio_codec_ctx, state.frame);
                        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
                        if (r < 0) break;
                        queue_audio(&state, state.frame);
                    }
                }
            }

            av_packet_unref(state.pkt);
        } else {
            // flush decoders at EOF
            if (avcodec_send_packet(state.video_codec_ctx, NULL) == 0) {
                while (avcodec_receive_frame(state.video_codec_ctx, state.frame) == 0) {
                    sws_scale(state.sws_ctx,
                              (const uint8_t *const *)state.frame->data,
                              state.frame->linesize,
                              0,
                              state.video_codec_ctx->height,
                              state.rgb_frame->data,
                              state.rgb_frame->linesize);
                    frame_to_ascii(state.rgb_frame,
                                   state.video_codec_ctx->width,
                                   state.video_codec_ctx->height);
                    usleep(33000);
                }
            }
            // audio
            if (state.audio_codec_ctx && avcodec_send_packet(state.audio_codec_ctx, NULL) == 0) {
                while (avcodec_receive_frame(state.audio_codec_ctx, state.frame) == 0) {
                    queue_audio(&state, state.frame);
                }
            }
            // wait a bit to let audio drain
            if (state.audio_dev) {
                while (SDL_GetQueuedAudioSize(state.audio_dev) > 0 && !state.quit) {
                    int ch2 = getch();
                    if (ch2 == 'q' || ch2 == 'Q') { state.quit = 1; break; }
                    usleep(20000);
                }
            }
            break; 
        }
    }

    cleanup_player(&state);
    endwin();
    printf("\n");
    return 0;
}

