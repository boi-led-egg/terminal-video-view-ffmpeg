// g++ ffmpeg-video-test.cpp -pedantic -Wall -o ffmpeg-test -lavformat -lavcodec -lavutil #-g
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <signal.h>
#include <iostream>
#include <thread>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

constexpr char records_extension[] = ".mp4";
constexpr int ffmpeg_log_level = AV_LOG_WARNING; // AV_LOG_INFO AV_LOG_DEBUG;
constexpr int socket_timeout = 10;
bool keep_running = true; // global for handling ctrl-c

std::string av_error2string(int ffmpeg_error)
{
    char msg[1024];
    av_strerror(ffmpeg_error, msg, 1024);
    return "Error " + std::to_string(ffmpeg_error) + ": " + std::string(msg);
}

void signal_handler(int s)
{
    // printf("Caught signal %d\n", s);
    keep_running = false;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_handler);
    if (argc <= 1) {
        fprintf(stderr, "No video or URL provided\n");
        exit(1);
    }
    const char* url = argv[1];
    // init FFMPEG
    avformat_network_init();
    av_log_set_level(ffmpeg_log_level);

    AVFormatContext* in_context = avformat_alloc_context();
    if (in_context == NULL) {
        fprintf(stderr, "Cannot allocate context\n");
        return -1;
    }

    const bool is_stream = (strncmp(url, "rtsp", 4) == 0 || strncmp(url, "http", 4) == 0 ||
                            strncmp(url, "RTSP", 4) == 0 || strncmp(url, "HTTP", 4) == 0);

    AVDictionary *opts = NULL;
    if (is_stream) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        // connection time out in microseconds for HTTP, in seconds for RTSP
        char timeout[32];
        if (socket_timeout <= 0) {
            strcpy(timeout, "0");
        } else {
            sprintf(timeout, "%d000000", socket_timeout);
        }
        av_dict_set(&opts, "stimeout", timeout, 0);
    }

    int ret = 0;
    if ((ret = avformat_open_input(&in_context, url, NULL, &opts)) != 0) {
        fprintf(stderr, "Cannot open url: %s\n", av_error2string(ret).c_str());
        return -1;
    }
    if (opts != NULL) {
        av_dict_free(&opts);
    }

    ret = avformat_find_stream_info(in_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot find stream info: %s\n", av_error2string(ret).c_str());
        return -1;
    }
    // Find the first video stream
    AVCodec* video_codec;
    int video_stream = av_find_best_stream(
        in_context,           // The media stream
        AVMEDIA_TYPE_VIDEO,   // The type of stream we are looking for - audio for example
        -1,                   // Desired stream number, -1 for any
        -1,                   // Number of related stream, -1 for none
        &video_codec,         // Gets the codec associated with the stream, can be NULL
        0                     // Flags - not used currently
        );
    if (video_stream != -1) {
        printf("Found video stream %d, resolution %dx%d, framerate %d/%d, codec name %s\n",
               video_stream,
               in_context->streams[video_stream]->codecpar->width,
               in_context->streams[video_stream]->codecpar->height,
               in_context->streams[video_stream]->avg_frame_rate.num,
               in_context->streams[video_stream]->avg_frame_rate.den,
               avcodec_get_name(in_context->streams[video_stream]->codecpar->codec_id));
    } else {
        fprintf(stderr, "Couldn't find a video stream\n");
        exit(1);
    }
    bool found_key_frame = false; // skip frames until first key frame found
    AVPacket* av_packet = av_packet_alloc();
    if (!av_packet) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        return 1;
    }

    // decoding part

    AVCodecContext *codecContext = avcodec_alloc_context3(video_codec);
    if (!codecContext) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    int result = avcodec_parameters_to_context(codecContext, in_context->streams[video_stream]->codecpar);
    if (result < 0) {
        fprintf(stderr, "cannot convert parameters to context\n");
        exit(1);
    }

    if (avcodec_open2(codecContext, video_codec, NULL) < 0){
        fprintf(stderr, "Cannot open the video codec\n");
        codecContext = nullptr;
    }

    AVFrame *frame;
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    int frame_num = 0;
    struct SwsContext *sws_ctx;

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int t_width = w.ws_col;
    int t_height = w.ws_row-1;

    int dst_w = 80;
    int dst_h = 25;
    int im_width = in_context->streams[video_stream]->codecpar->width;
    int im_height = in_context->streams[video_stream]->codecpar->height;
    if ((float)t_width / t_height <= (float)im_width / im_height * 2) {
        printf("image is wider, then the screen\n");
        dst_w = t_width;
        dst_h = floor((float)im_height / im_width * t_width);
    } else {
        printf("image is longer, then the screen\n");
        dst_w = floor((float)im_width / im_height * t_height * 2);
        dst_h = t_height * 2;
    }
    sws_ctx = sws_getContext(in_context->streams[video_stream]->codecpar->width,
                             in_context->streams[video_stream]->codecpar->height,
                             (AVPixelFormat)in_context->streams[video_stream]->codecpar->format,
                             dst_w, dst_h, AV_PIX_FMT_GRAY8,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "Cannot get sws context\n");
        exit(1);
    }
    uint8_t *dst_data[4];
    int dst_linesize[4];
    int dst_bufsize = av_image_alloc(dst_data, dst_linesize, dst_w, dst_h, AV_PIX_FMT_RGB24, 16);
    if (dst_bufsize < 0) {
        fprintf(stderr, "cannot allocate dst image\n");
        exit(1);
    }

    while (keep_running) {
        int status = 0;
        if ((status = av_read_frame(in_context, av_packet)) >= 0) {
            if (av_packet->stream_index != video_stream) {
                av_packet_unref(av_packet);
                continue;
            }
        }
        if (status == AVERROR(EAGAIN)) {
            printf("AGAIN frame\n");
            continue;
        } else if (status == AVERROR_EOF) {
            printf("EOF\n");
            break;
        } else if (status != 0) {
            printf("other read status %s\n", av_error2string(status).c_str());
            continue;
        }
        // skip frames before first key frame
        if (!found_key_frame && (av_packet->flags & AV_PKT_FLAG_KEY) == 1) {
            printf("Found key frame\n");
            found_key_frame = true;
        }

        if (found_key_frame) {
            int ret;
            ret = avcodec_send_packet(codecContext, av_packet);
            if (ret < 0) {
                fprintf(stderr, "Error sending a packet for decoding\n");
                exit(1);
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(codecContext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "Error during decoding\n");
                    exit(1);
                }
                sws_scale(sws_ctx, (const uint8_t * const*)frame->data,
                          frame->linesize,
                          0,
                          in_context->streams[video_stream]->codecpar->height,
                          dst_data,
                          dst_linesize);
                std::stringstream screen;
                screen << "\033[2J\033[1;1H";
                for (int y = 0; y < dst_h/2; y++) {
                    for (int x = 0; x < dst_w; x++) {
                        int u = 232 + dst_data[0][(2 * y + 0) * dst_linesize[0] + x] / 16;
                        int l = 232 + dst_data[0][(2 * y + 1) * dst_linesize[0] + x] / 16;
                        screen << "\033[38;5;" << u << "m\033[48;5;" << l << "m▀\033[m";
                    }
                    screen << std::endl;
                }
                screen << "[---------------------------------]";
                std::cout << screen.str();
                fflush(stdout);

                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        }
        av_packet_unref(av_packet);
        frame_num++;
    }

    av_packet_free(&av_packet);

    avformat_close_input(&in_context);
    avformat_free_context(in_context);
    return 0;
}
