//
//  player_base.cpp
//  fftest
//
//  Created by rei8 on 2019/10/18.
//  Copyright © 2019 lithium03. All rights reserved.
//

#include "player_base.hpp"

AVPacket flush_pkt;
AVPacket eof_pkt;
AVPacket abort_pkt;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void setParam(struct stream_param * param)
{
    Player *player = new Player();
    param->player = player;
    player->name = param->name;
    player->param = param;
}

void freeParam(struct stream_param * param)
{
    printf("freeParam\n");
    delete (Player *)param->player;
}

void quitPlayer(struct stream_param * param)
{
    printf("quitPlayer\n");
    auto player = (Player *)param->player;
    player->Quit();
}

double load_sound(void *arg, float *buffer, int num_packets)
{
    struct stream_param *param = (struct stream_param *)arg;
    auto player = (Player *)param->player;
    return player->load_sound(buffer, num_packets);
}

void seekPlayer(struct stream_param * param, int64_t pos)
{
    auto player = (Player *)param->player;
    player->seek(pos);
}

void seekPlayerChapter(struct stream_param * param, int inc)
{
    auto player = (Player *)param->player;
    player->seek_chapter(inc);
}

void cycleChancelPlayer(struct stream_param * param, int type)
{
    auto player = (Player *)param->player;
    switch (type) {
        case 0:
            player->stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
            break;
        case 1:
            player->stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
            break;
        case 2:
            player->stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
            break;

        default:
            break;
    }
}


void pausePlayer(struct stream_param * param, int state)
{
    auto player = (Player *)param->player;
    player->set_pause(state == 1);
}

int getPlayer_pause(struct stream_param * param)
{
    auto player = (Player *)param->player;
    if(player->pause) {
        return 1;
    }
    return 0;
}


int decode_thread(struct stream_param *stream);
int video_thread(Player *is);
int video_present_thread(Player *is);
void audio_thread(Player *is);
int subtitle_thread(Player *is);

int createParseThread(struct stream_param * param) {
    ((Player *)param->player)->parse_thread = std::thread(decode_thread, param);
    return 0;
}

int waitParseThread(struct stream_param * param) {
    ((Player *)param->player)->parse_thread.join();
    ((Player *)param->player)->Finalize();
    return ((Player *)param->player)->ret;
}


#ifdef __cplusplus
}
#endif /* __cplusplus */

Player::Player() : audio(this), subtitle(this), video(this)
{
}

void Player::Quit()
{
    quit = true;
    struct stream_param *p = (struct stream_param *)param;
    pause = p->sound_stop(p->stream) != 1;
    clear_soundbufer();
    {
        std::unique_lock<std::mutex> lk(video.pictq_mutex);
        video.pictq_cond.notify_one();
    }
    av_usleep(100*1000);
}

bool Player::IsQuit(bool pure)
{
    if (pure)
        return audio.audio_eof == AudioInfo::audio_eof_enum::eof && video.video_eof;
    return quit || (audio.audio_eof == AudioInfo::audio_eof_enum::eof && video.video_eof);
}

void Player::Finalize()
{
    Quit();
    struct stream_param *p = (struct stream_param *)param;
    p->sound_stop(p->stream);

    if(parse_thread.joinable()){
        parse_thread.join();
    }
    if(video_thread.joinable()){
        video_thread.join();
    }
    audio.cond_full.notify_all();
    if(audio_thread.joinable()){
        audio_thread.join();
    }
    if(subtitle_thread.joinable()){
        subtitle_thread.join();
    }
    if(display_thread.joinable()){
        display_thread.join();
    }
}

int decode_thread(struct stream_param *stream)
{
    av_log(NULL, AV_LOG_INFO, "decode_thread start\n");

    Player *player = (Player *)stream->player;
    player->ret = -1;
    
    double start_skip = stream->start_skip;
    
    int video_index = -1;
    int audio_index = -1;
    int subtitle_index = -1;
    
    player->video.videoStream = -1;
    player->audio.audioStream = -1;
    player->subtitle.subtitleStream = -1;

    player->pFormatCtx = avformat_alloc_context();
    unsigned char *buffer = (unsigned char *)av_malloc(1024*1024);
    AVIOContext *pIoCtx = avio_alloc_context(
                                             buffer,
                                             1024*1024,
                                             0,
                                             stream->stream,
                                             stream->read_packet,
                                             NULL,
                                             stream->seek
                                             );
    AVPacket packet = { 0 };
    bool error = false;
    player->pFormatCtx->pb = pIoCtx;
    char *filename = stream->name;
    
    av_log(NULL, AV_LOG_VERBOSE, "avformat_open_input()\n");
    // Open video file
    if (player->IsQuit() || avformat_open_input(&player->pFormatCtx, filename, NULL, NULL) != 0) {
        printf("avformat_open_input() failed %s\n", filename);
        goto failed_open;
    }
    
    av_log(NULL, AV_LOG_VERBOSE, "avformat_find_stream_info()\n");
    // Retrieve stream information
    if (player->IsQuit() || avformat_find_stream_info(player->pFormatCtx, NULL) < 0) {
        printf("avformat_find_stream_info() failed %s\n", filename);
        goto failed_run;
    }
    
    if(player->IsQuit()) {
        goto failed_run;
    }
    
    av_log(NULL, AV_LOG_VERBOSE, "av_dump_format()\n");
    // Dump information about file onto standard error
    av_dump_format(player->pFormatCtx, 0, filename, 0);
    
    if(player->IsQuit()) {
        goto failed_run;
    }
    
    for(unsigned int stream_index = 0; stream_index<player->pFormatCtx->nb_streams; stream_index++)
        player->pFormatCtx->streams[stream_index]->discard = AVDISCARD_ALL;

    if(player->IsQuit()) {
        goto failed_run;
    }
    
    av_log(NULL, AV_LOG_VERBOSE, "av_find_best_stream()\n");
    // Find the first video and audio stream
    video_index = av_find_best_stream(player->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_index = av_find_best_stream(player->pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    subtitle_index = av_find_best_stream(player->pFormatCtx, AVMEDIA_TYPE_SUBTITLE, -1, (audio_index >= 0 ? audio_index : video_index), NULL, 0);
    if (audio_index >= 0) {
        av_log(NULL, AV_LOG_VERBOSE, "audio stream open()\n");
        player->stream_component_open(audio_index);
    }
    if (video_index >= 0) {
        av_log(NULL, AV_LOG_VERBOSE, "video stream open()\n");
        player->stream_component_open(video_index);
    }
    if (subtitle_index >= 0) {
        av_log(NULL, AV_LOG_VERBOSE, "subtitle stream open()\n");
        player->stream_component_open(subtitle_index);
    }

    if(player->IsQuit()) {
        goto failed_run;
    }
    
    player->sync_type = Player::sync_source::sync_audio;
    if (player->video.videoStream < 0 || player->audio.audioStream < 0) {
        if (player->video.videoStream < 0) {
            av_log(NULL, AV_LOG_VERBOSE, "video missing\n");
            player->video.video_eof = true;
            stream->set_soundonly(stream->stream);
        }
        else {
            av_log(NULL, AV_LOG_VERBOSE, "audio missing\n");
            player->sync_type = Player::sync_source::sync_timer;
            player->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
        }
    }

    if(!player->video.video_eof)
        player->display_thread = std::thread(video_present_thread, player);
    
    stream->set_duration(stream->stream, player->get_duration());
    stream->wait_stop(stream->stream);

    // main decode loop
    av_log(NULL, AV_LOG_INFO, "decode_thread read loop\n");
    for (;;) {
        if (player->IsQuit()) {
            break;
        }
        // seek stuff goes here
        if(player->seek_req_type == Player::seek_type_next) {
            // if no chapter, skip to next media
            if(!player->pFormatCtx->nb_chapters) {
                if(stream->media_count > 1) {
                    av_log(NULL, AV_LOG_INFO, "next media\n");
                    player->Quit();
                }
                else {
                    player->seek_req_type = Player::seek_type_none;
                    player->seek_pos = AV_NOPTS_VALUE;
                    stream->wait_stop(stream->stream);
                    av_log(NULL, AV_LOG_INFO, "last media, ignore next\n");
                }
                continue;
            }
            
            // next chapter
            int64_t pos = (int64_t)(player->get_master_clock() * AV_TIME_BASE);
            AVRational timebase = { 1, AV_TIME_BASE };
            /* find the current chapter */
            int i;
            for (i = 0; i < player->pFormatCtx->nb_chapters; i++) {
                AVChapter *ch = player->pFormatCtx->chapters[i];
                if (av_compare_ts(pos, timebase, ch->start, ch->time_base) < 0) {
                    i--;
                    break;
                }
            }
            i++;
            i = FFMAX(i, 0);
            if (i >= player->pFormatCtx->nb_chapters) {
                if(stream->media_count > 1) {
                    av_log(NULL, AV_LOG_INFO, "next media\n");
                    player->Quit();
                }
                else {
                    player->seek_req_type = Player::seek_type_none;
                    player->seek_pos = AV_NOPTS_VALUE;
                    stream->wait_stop(stream->stream);
                    av_log(NULL, AV_LOG_INFO, "last media, ignore next\n");
                }
                continue;
            }
            player->seek_pos = av_rescale_q(player->pFormatCtx->chapters[i]->start, player->pFormatCtx->chapters[i]->time_base, timebase);
            player->seek_req_type = Player::seek_type_pos;
        }
        if(player->seek_req_type == Player::seek_type_prev) {
            // if no chapter, skip to start
            if(!player->pFormatCtx->nb_chapters) {
                av_log(NULL, AV_LOG_INFO, "go back to start\n");
                player->seek_pos = player->start_time_org;
                player->seek_req_type = Player::seek_type_pos;
                continue;
            }
            
            // previous chapter
            int64_t pos = (int64_t)((player->get_master_clock() - 5) * AV_TIME_BASE);
            AVRational timebase = { 1, AV_TIME_BASE };
            /* find the current chapter */
            int i;
            for (i = 0; i < player->pFormatCtx->nb_chapters; i++) {
                AVChapter *ch = player->pFormatCtx->chapters[i];
                if (av_compare_ts(pos, timebase, ch->start, ch->time_base) < 0) {
                    break;
                }
            }
            i--;
            if(i < 0) {
                av_log(NULL, AV_LOG_INFO, "go back to start\n");
                player->seek_pos = player->start_time_org;
                player->seek_req_type = Player::seek_type_pos;
            }
            else {
                player->seek_pos = av_rescale_q(player->pFormatCtx->chapters[i]->start, player->pFormatCtx->chapters[i]->time_base, timebase);
                player->seek_req_type = Player::seek_type_pos;
            }
            continue;
        }
        if (player->seek_req_type == Player::seek_type_pos) {
            auto seek_pos = player->seek_pos;
            if(seek_pos < 0) seek_pos = 0;
            AVRational timebase = { 1, AV_TIME_BASE };
            av_log(NULL, AV_LOG_INFO, "stream seek request receive %.2f(%lld)\n", (double)(seek_pos) * av_q2d(timebase), seek_pos);
            int stream_index = -1;
            int ret1 = av_seek_frame(player->pFormatCtx, stream_index, seek_pos, 0);
            if (ret1 < 0) {
                char buf[AV_ERROR_MAX_STRING_SIZE];
                char *errstr = av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret1);
                av_log(NULL, AV_LOG_ERROR, "error avformat_seek_file() %d %s\n", ret1, errstr);
                error = true;
            }
            if (ret1 >=0) {
                player->video.pictq_active_serial = player->subtitle.subpictq_active_serial = av_gettime();
                player->master_clock_offset = NAN;
                player->master_clock_start = AV_NOPTS_VALUE;
                if (player->audio.audioStream >= 0) {
                    player->audio.audio_eof = Player::AudioInfo::audio_eof_enum::playing;
                    av_log(NULL, AV_LOG_INFO, "audio flush request\n");
                    player->audio.audioq.flush();
                }
                if (player->video.videoStream >= 0) {
                    player->video.video_eof = false;
                    av_log(NULL, AV_LOG_INFO, "video flush request\n");
                    player->video.videoq.flush();
                }
                if (player->subtitle.subtitleStream >= 0) {
                    av_log(NULL, AV_LOG_INFO, "subtitle flush request\n");
                    player->subtitle.subtitleq.flush();
                }
            }
            player->seek_req_type = Player::seek_type_none;
            player->seek_pos = AV_NOPTS_VALUE;
            stream->wait_stop(stream->stream);
            continue;
        }

        if ((player->audio.audioStream < 0 && player->video.videoq.size > MAX_VIDEOQ_SIZE) ||
            (player->video.videoStream < 0 && player->audio.audioq.size > MAX_AUDIOQ_SIZE) ||
            (player->audio.audioq.size > MAX_AUDIOQ_SIZE || player->video.videoq.size > MAX_VIDEOQ_SIZE)) {
            av_usleep(10*1000);
            continue;
        }
        int ret1 = av_read_frame(player->pFormatCtx, &packet);
        if (ret1 < 0) {
            av_packet_unref(&packet);
            if (ret1 == AVERROR(EAGAIN))
                continue;
            if ((ret1 == AVERROR_EOF) || (ret1 = AVERROR(EIO))) {
                if (error || player->pFormatCtx->pb->eof_reached) {
                    if (ret1 == AVERROR_EOF) {
                        av_log(NULL, AV_LOG_INFO, "decoder EOF\n");
                    }
                    else {
                        av_log(NULL, AV_LOG_INFO, "decoder I/O Error\n");
                    }
                    if (player->video.videoStream >= 0) {
                        av_log(NULL, AV_LOG_INFO, "video EOF request\n");
                        player->video.videoq.putEOF();
                    }
                    if (player->audio.audioStream >= 0) {
                        av_log(NULL, AV_LOG_INFO, "audio EOF request\n");
                        player->audio.audioq.putEOF();
                    }
                    if (player->subtitle.subtitleStream >= 0) {
                        av_log(NULL, AV_LOG_INFO, "subtitle EOF request\n");
                        player->subtitle.subtitleq.putEOF();
                    }
                    
                    while (!(player->IsQuit() || player->seek_req_type != Player::seek_type_none)) {
                        av_usleep(100*1000);
                    }
                    
                    if (player->seek_req_type != Player::seek_type_none) continue;
                    break;
                }
                error = true;
            }
            char buf[AV_ERROR_MAX_STRING_SIZE];
            char *errstr = av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret1);
            av_log(NULL, AV_LOG_ERROR, "error av_read_frame() %d %s\n", ret1, errstr);
            continue;
        }
        error = false;
        if (player->start_time_org == AV_NOPTS_VALUE) {
            if(player->pFormatCtx->start_time != AV_NOPTS_VALUE)
                player->start_time_org = player->pFormatCtx->start_time;
            else
                player->start_time_org = 0;
        }
        if (isnan(player->master_clock_offset)) {
            player->master_clock_start = av_gettime();
            AVRational timebase = { 1, AV_TIME_BASE };
            player->master_clock_offset = packet.pts * av_q2d(player->pFormatCtx->streams[packet.stream_index]->time_base) - player->start_time_org * av_q2d(timebase);
        }

        if (packet.stream_index == player->video.videoStream) {
            player->video.video_eof = false;
            player->video.videoq.put(&packet);
        }
        else if (packet.stream_index == player->audio.audioStream) {
            player->audio.audio_eof = Player::AudioInfo::audio_eof_enum::playing;
            player->audio.audioq.put(&packet);
        }
        else if (packet.stream_index == player->subtitle.subtitleStream) {
            player->subtitle.subtitleq.put(&packet);
        }
        else {
            av_packet_unref(&packet);
        }
        
        if (!isnan(start_skip)) {
            start_skip -= 1;
            if(start_skip > 0) {
                printf("start skip %f sec\n", start_skip);
                player->seek_pos = (int64_t)(start_skip * AV_TIME_BASE);
                if (player->pFormatCtx->start_time != AV_NOPTS_VALUE) {
                    player->seek_pos += player->pFormatCtx->start_time;
                }
                player->seek_req_type = Player::seek_type_pos;
            }
            start_skip = NAN;
        }
    }
    av_log(NULL, AV_LOG_INFO, "decode_thread read loop end\n");
    /* all done - wait for it */
    while (!player->IsQuit(true)) {
        av_usleep(100*1000);
    }
    
finish:
    av_log(NULL, AV_LOG_INFO, "decode_thread end\n");
    player->ret = 0;
failed_run:
    avformat_close_input(&player->pFormatCtx);
failed_open:
    av_freep(&pIoCtx);
    return player->ret;
}

int Player::stream_component_open(int stream_index)
{
    std::shared_ptr<AVCodecContext> codecCtx;
    AVCodec *codec;
    struct stream_param *p = (struct stream_param *)param;
    
    if (stream_index < 0 || (unsigned)stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }
    
    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_PANIC, "Unsupported codec!\n");
        return -1;
    }
    
    codecCtx = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(codec), [](AVCodecContext *ptr) {avcodec_free_context(&ptr); });
    if (avcodec_parameters_to_context(codecCtx.get(), pFormatCtx->streams[stream_index]->codecpar) < 0) {
        av_log(NULL, AV_LOG_PANIC, "Couldn't copy codec parameter to codec context\n");
        return -1;
    }
    codecCtx->time_base = pFormatCtx->streams[stream_index]->time_base;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "threads", "auto", 0);
    
    if (avcodec_open2(codecCtx.get(), codec, &opts) < 0) {
        av_log(NULL, AV_LOG_PANIC, "Unsupported codec!\n");
        return -1;
    }
    av_dict_free(&opts);
    
    AVDictionaryEntry *lang = av_dict_get(pFormatCtx->streams[stream_index]->metadata, "language", NULL,0);
    pFormatCtx->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    
    switch (codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            audio.audioStream = stream_index;
            audio.audio_st = pFormatCtx->streams[stream_index];
            audio.audio_ctx = codecCtx;
            audio.audio_eof = AudioInfo::audio_eof_enum::playing;
            audio.audio_filter_src = AudioInfo::AudioParams();
            if(lang) {
                audio.language = lang->value;
            }
            else {
                audio.language = "";
            }
            audio_thread = std::thread(::audio_thread, this);
            if(!pause) {
                pause = p->sound_play(p->stream) != 1;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            video.videoStream = stream_index;
            video.video_st = pFormatCtx->streams[stream_index];
            video.video_ctx = codecCtx;
            if(lang) {
                video.language = lang->value;
            }
            else {
                video.language = "";
            }
            
            video.video_clock_start = NAN;
            video.video_clock = NAN;
            video.frame_timer = NAN;
            video.frame_last_pts = NAN;
            video.frame_last_delay = 10e-3;

        {
            video.video_SAR = video.video_ctx->sample_aspect_ratio;
            double aspect_ratio = 0;
            if (video.video_ctx->sample_aspect_ratio.num == 0) {
                aspect_ratio = 0;
            }
            else {
                aspect_ratio = av_q2d(video.video_ctx->sample_aspect_ratio) *
                video.video_ctx->width / video.video_ctx->height;
            }
            if (aspect_ratio <= 0.0) {
                aspect_ratio = (double)video.video_ctx->width /
                (double)video.video_ctx->height;
            }
            
            video.video_height = codecCtx->height;
            video.video_width = ((int)rint(video.video_height * aspect_ratio)) & ~1;
                        
            // initialize SWS context for software scaling
            video.sws_ctx = std::shared_ptr<SwsContext>(
                                                  sws_getCachedContext(NULL,
                                                                       codecCtx->width,
                                                                       codecCtx->height,
                                                                       codecCtx->pix_fmt,
                                                                       video.video_width,
                                                                       video.video_height,
                                                                       AV_PIX_FMT_RGBA,
                                                                       SWS_BICUBLIN,
                                                                       NULL, NULL, NULL
                                                                       ), &sws_freeContext);
            video.video_srcheight = codecCtx->height;
            video.video_srcwidth = codecCtx->width;
        }
            video.video_eof = false;
            video_thread = std::thread(::video_thread, this);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            subtitle.subtitleStream = stream_index;
            subtitle.subtitle_st = pFormatCtx->streams[stream_index];
            subtitle.subtitle_ctx = codecCtx;
            if(lang) {
                subtitle.language = lang->value;
            }
            else {
                subtitle.language = "";
            }
            subtitle_thread = std::thread(::subtitle_thread, this);
            break;
        default:
            pFormatCtx->streams[stream_index]->discard = AVDISCARD_ALL;
            break;
    }
    return 0;
}

void Player::stream_component_close(int stream_index)
{
    AVFormatContext *ic = pFormatCtx;
    
    if (stream_index < 0 || (unsigned int)stream_index >= ic->nb_streams)
        return;
    
    AVCodecParameters *codecpar = ic->streams[stream_index]->codecpar;
    
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            audio.audioq.AbortQueue();
            clear_soundbufer();
            if(audio_thread.joinable())
                audio_thread.join();
            clear_soundbufer();
            break;
        case AVMEDIA_TYPE_VIDEO:
            video.videoq.AbortQueue();
            destory_pictures();
            if (video_thread.joinable())
                video_thread.join();
            destory_pictures();
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            subtitle.subtitleq.AbortQueue();
            if (subtitle_thread.joinable())
                subtitle_thread.join();
            subtitle.subpictq.clear();
            break;
        default:
            break;
    }
    
    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            audio.audio_st = NULL;
            audio.audioStream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            video.video_st = NULL;
            video.videoStream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            subtitle.subtitle_st = NULL;
            subtitle.subtitleStream = -1;
            break;
        default:
            break;
    }
}

bool Player::Configure_VideoFilter(AVFilterContext **filt_in, AVFilterContext **filt_out, AVFrame *frame, AVFilterGraph *graph)
{
    AVFilterContext *filt_deint = NULL;
    char args[256];
    
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             video.video_st->time_base.num, video.video_st->time_base.den,
             video.video_st->codecpar->sample_aspect_ratio.num, FFMAX(video.video_st->codecpar->sample_aspect_ratio.den, 1));
    AVRational fr = av_guess_frame_rate(pFormatCtx, video.video_st, NULL);
    if (fr.num && fr.den)
        av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d", fr.num, fr.den);
    
    if (avfilter_graph_create_filter(
                                     filt_in,
                                     avfilter_get_by_name("buffer"),
                                     "buffer",
                                     args,
                                     NULL,
                                     graph
                                     ) < 0)
        return false;
    if (avfilter_graph_create_filter(
                                     filt_out,
                                     avfilter_get_by_name("buffersink"),
                                     "buffersink",
                                     NULL,
                                     NULL,
                                     graph
                                     ) < 0)
        return false;
    
    auto filt_input = *filt_in;
    if (video.deinterlace) {
        snprintf(args, sizeof(args), "mode=0:parity=-1:deint=1");
        if (avfilter_graph_create_filter(
                                         &filt_deint,
                                         avfilter_get_by_name("bwdif"),
                                         "deinterlace",
                                         args,
                                         NULL,
                                         graph
                                         ) < 0)
            return false;
        if (avfilter_link(filt_input, 0, filt_deint, 0) != 0)
            return false;
        filt_input = filt_deint;
    }
    if (avfilter_link(filt_input, 0, *filt_out, 0) != 0)
        return false;
    return (avfilter_graph_config(graph, NULL) >= 0);
}

double Player::synchronize_video(AVFrame *src_frame, double pts, double framerate)
{
    double frame_delay;
    
    if (!isnan(pts)) {
        /* if we have pts, set video clock to it */
        video.video_clock = pts;
    }
    else {
        /* if we aren't given a pts, set it to the clock */
        pts = video.video_clock;
    }
    if(isnan(pts))
        pts = 0;
    /* update the video clock */
    frame_delay = 1 / framerate;
    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    video.video_clock += frame_delay;
    return video.video_clock;
}

int video_thread(Player *is)
{
    av_log(NULL, AV_LOG_INFO, "video_thread start\n");
    AVPacket packet = { 0 }, *inpkt = &packet;
    AVCodecContext *video_ctx = is->video.video_ctx.get();
    AVFrame frame = { 0 }, *inframe = &frame;
    std::shared_ptr<AVFilterGraph> graph(avfilter_graph_alloc(), [](AVFilterGraph *ptr) { avfilter_graph_free(&ptr); });
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    AVPixelFormat last_format = (AVPixelFormat)-2;
    int64_t last_serial = 0, serial = 0;
    is->video.pictq_active_serial = 0;
    AVRational frame_rate = av_guess_frame_rate(is->pFormatCtx, is->video.video_st, NULL);
    struct stream_param *stream = (struct stream_param *)is->param;
    
    
    switch (is->video.video_ctx->codec_id)
    {
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_MJPEGB:
        case AV_CODEC_ID_LJPEG:
            is->video.deinterlace = false;
            break;
        default:
            is->video.deinterlace = true;
            break;
    }
    
    av_log(NULL, AV_LOG_INFO, "video_thread read loop\n");
    std::deque<double> lastpts;
    double pts = NAN;
    double prevpts = NAN;
    while (true) {
        if (is->video.video_eof) {
            while (is->video.videoq.get(&packet, 1) == 0)
                ;
        }
        else if (is->video.videoq.get(&packet, 1) < 0) {
            // means we quit getting packets
            av_log(NULL, AV_LOG_INFO, "video Quit\n");
            is->video.video_eof = true;
            packet = { 0 };
            goto finish;
        }
        if (is->IsQuit())
            goto finish;
        
        if (packet.data == flush_pkt.data) {
            av_log(NULL, AV_LOG_INFO, "video buffer flush\n");
            avcodec_flush_buffers(video_ctx);
            packet = { 0 };
            inpkt = &packet;
            inframe = &frame;
            is->video.video_eof = false;
            serial = av_gettime();
            is->video.pictq_active_serial = serial;
            continue;
        }
        if (packet.data == eof_pkt.data) {
            av_log(NULL, AV_LOG_INFO, "video buffer EOF\n");
            packet = { 0 };
            inpkt = NULL;
        }
        if (packet.data == abort_pkt.data) {
            av_log(NULL, AV_LOG_INFO, "video buffer ABORT\n");
            is->video.video_eof = true;
            packet = { 0 };
            goto finish;
        }
        // send packet to codec context
        if (avcodec_send_packet(video_ctx, inpkt) >= 0) {
            
            // Decode video frame
            int ret;
            while ((ret = avcodec_receive_frame(video_ctx, &frame)) >= 0 || ret == AVERROR_EOF) {
                if (ret == AVERROR_EOF){
                    av_log(NULL, AV_LOG_INFO, "video EOF\n");
                    inframe = NULL;
                }
                
                if (inframe) {
                    if (frame.width != last_w ||
                        frame.height != last_h ||
                        frame.format != last_format ||
                        last_serial != serial) {
                        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(), [](AVFilterGraph *ptr) { avfilter_graph_free(&ptr); });
                        if (!is->Configure_VideoFilter(&filt_in, &filt_out, &frame, graph.get())) {
                            is->Quit();
                            goto failed;
                        }
                        last_w = frame.width;
                        last_h = frame.height;
                        last_format = (AVPixelFormat)frame.format;
                        last_serial = serial;
                    }
                }
                
                
                if (inframe && av_buffersrc_write_frame(filt_in, inframe) < 0)
                    goto failed;
                
                if (inframe) av_frame_unref(inframe);
                if (!filt_out) break;
                while (av_buffersink_get_frame(filt_out, &frame) >= 0) {
                    
                    int screen_width = stream->get_width(stream->stream);
                    int screen_height = stream->get_height(stream->stream);
                    
                    if (frame.width != is->video.video_srcwidth || frame.height != is->video.video_srcheight
                        || frame.sample_aspect_ratio.den != is->video.video_SAR.den || frame.sample_aspect_ratio.num != is->video.video_SAR.num
                        || screen_width != is->video.screen_width ||
                            screen_height != is->video.screen_height)
                    {
                        is->video.screen_width = screen_width;
                        is->video.screen_height = screen_height;
                        is->video.video_SAR = frame.sample_aspect_ratio;
                        is->video.video_aspect = av_q2d(frame.sample_aspect_ratio);
                        is->video.sws_ctx = NULL;
                        double aspect_ratio = 0;
                        if (video_ctx->sample_aspect_ratio.num == 0) {
                            aspect_ratio = 0;
                        }
                        else {
                            aspect_ratio = av_q2d(video_ctx->sample_aspect_ratio) *
                            frame.width / frame.height;
                        }
                        if (aspect_ratio <= 0.0) {
                            aspect_ratio = (double)frame.width /
                            (double)frame.height;
                        }
                        is->video.video_height = video_ctx->height;
                        is->video.video_width = ((int)rint(is->video.video_height * aspect_ratio)) & ~1;

                        // initialize SWS context for software scaling
                        is->video.video_srcheight = frame.height;
                        is->video.video_srcwidth = frame.width;
                        is->video.sws_ctx = std::shared_ptr<SwsContext>(
                                                                  sws_getCachedContext(NULL,
                                                                                       is->video.video_srcwidth,
                                                                                       is->video.video_srcheight,
                                                                                       video_ctx->pix_fmt,
                                                                                       is->video.video_width,
                                                                                       is->video.video_height,
                                                                                       AV_PIX_FMT_RGBA,
                                                                                       SWS_BICUBLIN, NULL, NULL, NULL
                                                                                       ), &sws_freeContext);
                    } //if src.size != frame.size
                    
                    int64_t pts_t;
                    if ((pts_t = frame.best_effort_timestamp) != AV_NOPTS_VALUE) {
                        pts = pts_t * av_q2d(is->video.video_st->time_base);
                        pts -= av_q2d(AV_TIME_BASE_Q) * is->start_time_org;
                        //av_log(NULL, AV_LOG_INFO, "video clock %f\n", pts);
                        
                        if (isnan(is->video.video_clock_start)) {
                            is->video.video_clock_start = pts;
                        }
                    }
                    else {
                        pts = NAN;
                    }
                    
                    if (!isnan(stream->play_duration) && is->sync_type == Player::sync_timer) {
                        if (pts - is->video.video_clock_start > stream->play_duration) {
                            is->Quit();
                        }
                    }
                    
                    if (!isnan(pts)) {
                        lastpts.push_back(pts);
                    }
                    
                    if (fabs(prevpts - pts) < 1.0e-6) {
                        if (lastpts.size() > 1) {
                            double p = lastpts.front();
                            double dpts = 0;
                            for (auto i : lastpts) {
                                dpts += i - p;
                                p = i;
                            }
                            dpts /= (lastpts.size() - 1);
                            
                            pts += dpts;
                        }
                    }
                    if (!isnan(pts))
                        prevpts = pts;
                    if (lastpts.size() > 30)
                        lastpts.pop_front();
                    
                    frame_rate = filt_out->inputs[0]->frame_rate;
                    pts = is->synchronize_video(&frame, pts, av_q2d(frame_rate));
                    if (is->queue_picture(&frame, pts) < 0) {
                        goto finish;
                    }
                    av_frame_unref(&frame);
                } //while(av_buffersink_get_frame)
                
                if (!inframe) {
                    goto finish;
                }
            } //while(avcodec_receive_frame)
            
            av_frame_unref(&frame);
        } //if(avcodec_send_packet)
        
        if (inpkt) av_packet_unref(inpkt);
        if (!inframe) {
            is->video.video_eof = true;
        }
    }//while(true)
failed:
    av_log(NULL, AV_LOG_INFO, "video_thread failed\n");

finish:
    av_log(NULL, AV_LOG_INFO, "video_thread end\n");
    is->video.video_eof = true;
    return 0;
}

int Player::queue_picture(AVFrame *pFrame, double pts)
{
    //printf("queue_picture:%f\n", pts);
    /* wait until we have space for a new pic */
    {
        std::unique_lock<std::mutex> lk(video.pictq_mutex);
        video.pictq_cond.wait(lk, [this] { return IsQuit() || video.pictq_size < VIDEO_PICTURE_QUEUE_SIZE; });
    }
    if (IsQuit())
        return -1;
    
    // windex is set to 0 initially
    VideoPicture *vp = &video.pictq[video.pictq_windex];
    
    int width = video.video_width;
    int height = video.video_height;
    
    /* allocate or resize the buffer! */
    if (!vp->allocated ||
        vp->width != width ||
        vp->height != height) {
        
        vp->Allocate(width, height);
        if (IsQuit()) {
            return -1;
        }
    }
    
    /* We have a place to put our picture on the queue */
    // Convert the image into YUV format that SDL uses
    sws_scale(video.sws_ctx.get(), pFrame->data,
              pFrame->linesize, 0, pFrame->height,
              vp->bmp.data, vp->bmp.linesize);
    
    vp->pts = pts;
    vp->serial = av_gettime();
    
    /* now we inform our display thread that we have a pic ready */
    if (++video.pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
        video.pictq_windex = 0;
    }
    std::unique_lock<std::mutex> lk(video.pictq_mutex);
    video.pictq_size++;
    return 0;
}

int video_present_thread(Player *is)
{
    av_log(NULL, AV_LOG_INFO, "video_present_thread start\n");
    struct stream_param *param = (struct stream_param *)is->param;
    bool waiting = false;
    
    while (!is->IsQuit()) {
        if (is->pause) {
            av_usleep(250*1000);
            continue;
        }
        
        if (isnan(is->get_master_clock())) {
            if(!waiting) {
                waiting = true;
                param->wait_start(param->stream);
            }
            av_usleep(100*1000);
            continue;
        }
        
        if (is->video.pictq_size == 0) {
            if(!waiting) {
                waiting = true;
                param->wait_start(param->stream);
            }
            av_usleep(100*1000);
            continue;
        }
        
        VideoPicture *vp = &is->video.pictq[is->video.pictq_rindex];
        if(!vp->allocated) {
            if(!waiting) {
                waiting = true;
                param->wait_start(param->stream);
            }
            av_usleep(100*1000);
            continue;
        }
        while (vp->serial < is->video.pictq_active_serial && is->video.pictq_size > 0) {
            vp = is->next_picture_queue();
            is->video.frame_last_pts = NAN;
        }
        if(!vp->allocated) {
            if(!waiting) {
                waiting = true;
                param->wait_start(param->stream);
            }
            av_usleep(100*1000);
            continue;
        }

        if(waiting && is->seek_req_type == Player::seek_type_none) {
            waiting = false;
            param->wait_stop(param->stream);
        }
        
        if (isnan(is->video.frame_timer)) {
            is->video.frame_timer = (double)av_gettime() / 1000000.0;
        }
        
        double ref_clock = is->get_master_clock();
        double diff = vp->pts - ref_clock;

        diff += param->latency;
        //printf("diff:%f\n",diff);
        if (diff < 0) {
            is->video.frame_last_pts = vp->pts;
            vp = is->next_picture_queue();
            continue;
        }

        double delay = vp->pts - is->video.frame_last_pts; /* the pts from last time */
        if (delay >= -1.0 && delay <= 1.0) {
            // use original
        }
        else {
            /* if incorrect delay, use previous one */
            delay = is->video.frame_last_delay;
        }
        /* save for next time */
        is->video.frame_last_pts = vp->pts;
        
        /* Skip or repeat the frame. Take delay into account
         FFPlay still doesn't "know if this is the best guess." */
        double sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
        if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
            if (diff <= -sync_threshold) {
                delay = (diff + delay < 0)? 0: diff + delay;
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                delay = diff + delay;
            }
            else if (diff >= sync_threshold * 2) {
                delay = 2 * delay;
            }
            else if (diff >= sync_threshold) {
                delay = (diff / sync_threshold) * delay;
            }
        }

        if (delay >= -1.0 && delay <= 1.0) {
            is->video.frame_last_delay = (is->video.frame_last_delay * 9 + delay * 1) / 10;
        }

        //printf("delay:%f\n",delay);
        is->video.frame_timer += delay;
        /* computer the REAL delay */
        double actual_delay = is->video.frame_timer - av_gettime() / 1000000.0;
        
        //printf("actual_delay:%f\n",actual_delay);
        if (fabs(actual_delay) > AV_NOSYNC_THRESHOLD) {
            // ignore delay
            is->video.frame_timer = av_gettime() / 1000000.0;
        }
        else if (actual_delay > AV_SYNC_THRESHOLD) {
            //printf("wait:%f\n",actual_delay);
            av_usleep((actual_delay - AV_SYNC_THRESHOLD) * 1000000);
        }

        is->video_display(vp);
        vp = is->next_picture_queue();
    }

    av_log(NULL, AV_LOG_INFO, "video_present_thread finish\n");
    return 0;
}

void Player::video_display(VideoPicture *vp)
{
    if (vp->allocated) {
        subtitle_display(vp);
        struct stream_param *stream = (struct stream_param *)param;
        stream->draw_pict(stream->stream, vp->bmp.data[0], vp->width, vp->height, vp->bmp.linesize[0], get_master_clock());
    }
}

VideoPicture *Player::next_picture_queue()
{
    if(video.pictq_size <= 0)
        return &video.pictq[video.pictq_rindex];

    video.pictq_prev = &video.pictq[video.pictq_rindex];
    /* update queue for next picture! */
    if (++video.pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
        video.pictq_rindex = 0;
    }
    std::unique_lock<std::mutex> lk(video.pictq_mutex);
    video.pictq_size--;
    video.pictq_cond.notify_one();
    return &video.pictq[video.pictq_rindex];
}

void Player::destory_pictures()
{
    std::unique_lock<std::mutex> lk(video.pictq_mutex);
    for (VideoPicture *p = video.pictq; p < &video.pictq[VIDEO_PICTURE_QUEUE_SIZE]; p++) {
        if (p == &video.pictq[video.pictq_windex]) continue;
        p->Free();
        p->pts = 0;
    }
    video.pictq_size = 1;
    video.pictq_cond.notify_one();
}

void Player::destory_all_pictures()
{
    std::unique_lock<std::mutex> lk(video.pictq_mutex);
    for (VideoPicture *p = video.pictq; p < &video.pictq[VIDEO_PICTURE_QUEUE_SIZE]; p++) {
        p->Free();
        p->pts = 0;
    }
    video.pictq_size = 0;
    video.pictq_cond.notify_one();
}

double Player::get_master_clock()
{
    if(sync_type == sync_audio) {
        return audio_clock_base + (av_gettime() - audio_last_call) / 1000000.0;
    }
    else if(sync_type == sync_timer) {
        return (av_gettime() - master_clock_start) / 1000000.0 + master_clock_offset;
    }
    return (av_gettime() - master_clock_start) / 1000000.0 + master_clock_offset;
}

double Player::get_duration()
{
    if(!pFormatCtx){
        return 0;
    }
    if (pFormatCtx->duration) {
        return pFormatCtx->duration / 1000000.0;
    }
    else if (video.videoStream >= 0 && pFormatCtx->streams[video.videoStream]->duration) {
        return pFormatCtx->streams[video.videoStream]->duration / 1000000.0;
    }
    else if (audio.audioStream >= 0 && pFormatCtx->streams[audio.audioStream]->duration) {
        return pFormatCtx->streams[audio.audioStream]->duration / 1000000.0;
    }
    else {
        return 0;
    }
}

double Player::load_sound(float *buffer, int num_packet)
{
    int ch = audio.out_channels;
    if(pause || audio.read_idx + num_packet >= audio.write_idx) {
        //printf("silence\n");
        memset(buffer, 0, sizeof(float)*num_packet*ch);
        audio_last_call = av_gettime();
        audio_clock_base = audio.pts_base - (double)(audio.write_idx - audio.read_idx) / audio.sample_rate;
        return audio_clock_base;
    }

    int offset = audio.read_idx % audio.buf_length;
    if(offset + num_packet > audio.buf_length) {
        int len = audio.buf_length - offset;
        memcpy(buffer, &audio.audio_wav[offset*ch], sizeof(float)*len*ch);
        audio.read_idx += len;
        // wrap-arround
        num_packet -= len;
        memcpy(&buffer[len*ch], audio.audio_wav, sizeof(float)*num_packet*ch);
        audio.read_idx += num_packet;
    }
    else {
        memcpy(buffer, &audio.audio_wav[offset*ch], sizeof(float)*num_packet*ch);
        audio.read_idx += num_packet;
    }
    {
        std::unique_lock<std::mutex> lk(audio.audio_mutex);
        audio.cond_full.notify_one();
        audio_last_call = av_gettime();
        audio_clock_base = audio.pts_base - (double)(audio.write_idx - audio.read_idx) / audio.sample_rate;
    }

    struct stream_param *stream = (struct stream_param *)param;
    if (!isnan(stream->play_duration)) {
        double s = 0;
        if (!isnan(stream->start_skip)) {
            s = stream->start_skip;
        }
        if (audio_clock_base - s > stream->play_duration) {
            Quit();
        }
    }
    
    return audio_clock_base;
}

static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

void audio_thread(Player *is)
{
    av_log(NULL, AV_LOG_INFO, "audio_thread start\n");
    is->sync_type = Player::sync_audio;
    AVCodecContext *aCodecCtx = is->audio.audio_ctx.get();
    AVPacket pkt = { 0 }, *inpkt = &pkt;
    AVFrame audio_frame_in = { 0 }, *inframe = &audio_frame_in;
    AVFrame audio_frame_out = { 0 };
    std::shared_ptr<AVFilterGraph> graph(avfilter_graph_alloc(), [](AVFilterGraph *ptr) { avfilter_graph_free(&ptr); });
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    struct stream_param *stream = (struct stream_param *)is->param;
    
    while(true) {
        int ret;
        if (inpkt) {
            if ((ret = is->audio.audioq.get(inpkt, 1)) < 0) {
                av_log(NULL, AV_LOG_INFO, "audio Quit\n");
                is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
                goto quit_audio;
            }
            if (is->audio.audio_eof == Player::AudioInfo::audio_eof_enum::playing && ret == 0) {
                continue;
            }
            if (inpkt->data == flush_pkt.data) {
                av_log(NULL, AV_LOG_INFO, "audio buffer flush\n");
                avcodec_flush_buffers(aCodecCtx);
                is->clear_soundbufer();
                pkt = { 0 };
                inpkt = &pkt;
                inframe = &audio_frame_in;
                continue;
            }
            if (inpkt->data == eof_pkt.data) {
                av_log(NULL, AV_LOG_INFO, "audio buffer EOF\n");
                is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::input_eof;
            }
            if (inpkt->data == abort_pkt.data) {
                av_log(NULL, AV_LOG_INFO, "audio buffer ABORT\n");
                is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
                goto quit_audio;
            }
        }
        if (is->audio.audio_eof >= Player::AudioInfo::audio_eof_enum::input_eof) {
            inpkt = NULL;
            if (is->audio.audio_eof == Player::AudioInfo::audio_eof_enum::output_eof) {
                is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
                av_log(NULL, AV_LOG_INFO, "audio EOF\n");
                goto quit_audio;
            }
        }
        
        // send packet to codec context
        ret = avcodec_send_packet(aCodecCtx, inpkt);
        if (ret >= 0 || (is->audio.audio_eof == Player::AudioInfo::audio_eof_enum::input_eof && ret == AVERROR_EOF)) {
            if (inpkt) av_packet_unref(inpkt);
            
            // Decode audio frame
            while ((ret = avcodec_receive_frame(aCodecCtx, inframe)) >= 0 || ret == AVERROR_EOF) {
                if (ret == AVERROR_EOF)
                    inframe = NULL;
                
                if (inframe) {
                    auto dec_channel_layout = get_valid_channel_layout(inframe->channel_layout, inframe->channels);
                    if (!dec_channel_layout)
                        dec_channel_layout = av_get_default_channel_layout(inframe->channels);
                    bool reconfigure =
                    cmp_audio_fmts(is->audio.audio_filter_src.fmt, is->audio.audio_filter_src.channels,
                                   (enum AVSampleFormat)inframe->format, inframe->channels) ||
                    is->audio.audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio.audio_filter_src.freq != inframe->sample_rate;
                    
                    if (reconfigure) {
                        av_log(NULL, AV_LOG_INFO, "audio reconfigure\n");
                        is->audio.audio_filter_src.fmt = (enum AVSampleFormat)inframe->format;
                        is->audio.audio_filter_src.channels = inframe->channels;
                        is->audio.audio_filter_src.channel_layout = dec_channel_layout;
                        is->audio.audio_filter_src.freq = inframe->sample_rate;
                        
                        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(), [](AVFilterGraph *ptr) { avfilter_graph_free(&ptr); });
                        if (!is->configure_audio_filters(&filt_in, &filt_out, graph.get(), is->audio.audio_filter_src))
                            goto failed_audio;
                    }
                }
                
                if (!filt_in || !filt_out)
                    goto failed_audio;
                
                if (av_buffersrc_add_frame(filt_in, inframe) < 0) {
                    av_log(NULL, AV_LOG_INFO, "audio av_buffersrc_add_frame() failed\n");
                    goto failed_audio;
                }
                
                if (inframe) av_frame_unref(inframe);
                while ((ret = av_buffersink_get_frame(filt_out, &audio_frame_out)) >= 0) {
                    auto pts_t = audio_frame_out.best_effort_timestamp;
                    double pts = NAN;
                    if (pts_t != AV_NOPTS_VALUE) {
                        pts = av_q2d(is->audio.audio_st->time_base)*pts_t;
                        pts -= av_q2d(AV_TIME_BASE_Q) * is->start_time_org;
                    }
                    //av_log(NULL, AV_LOG_INFO, "audio pts %f\n", pts);

                    float *audio_buf = (float *)audio_frame_out.data[0];
                    int out_samples = audio_frame_out.nb_samples;
                    {
                        std::unique_lock<std::mutex> lk(is->audio.audio_mutex);
                        is->audio.cond_full.wait(lk, [is, out_samples] { return is->IsQuit() || is->audio.write_idx + out_samples < is->audio.read_idx + is->audio.buf_length; });

                        if (is->IsQuit()) {
                            is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
                            goto quit_audio;
                        }

                        int ch = is->audio.out_channels;
                        int offset = is->audio.write_idx % is->audio.buf_length;
                        if(offset + out_samples > is->audio.buf_length) {
                            int len = is->audio.buf_length - offset;
                            memcpy(&is->audio.audio_wav[offset*ch], audio_buf, sizeof(float)*len*ch);
                            // wrap-arround
                            is->audio.write_idx += len;
                            out_samples -= len;
                            memcpy(is->audio.audio_wav, &audio_buf[len*ch], sizeof(float)*out_samples*ch);
                            is->audio.write_idx += out_samples;
                        }
                        else {
                            memcpy(&is->audio.audio_wav[offset*ch], audio_buf, sizeof(float)*out_samples*ch);
                            is->audio.write_idx += out_samples;
                        }
                        if(!isnan(pts)) {
                            is->audio.pts_base = pts;
                        }
                    }
                    av_frame_unref(&audio_frame_out);
                }//while(av_buffersink_get_frame)
                
                if (ret == AVERROR_EOF) {
                    is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::output_eof;
                    av_log(NULL, AV_LOG_INFO, "audio output EOF\n");
                }
                
                if(!inframe) {
                    goto quit_audio;
                }
            }//while(avcodec_receive_frame)
        } //if (avcodec_send_packet)
        else {
            if(inpkt) av_packet_unref(inpkt);
        }
        
        if (is->IsQuit()) {
            is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
            goto quit_audio;
        }
    }//while(true)
failed_audio:
    av_log(NULL, AV_LOG_INFO, "audio_thread failed\n");
    is->pause = stream->sound_stop(stream->stream) != 1;
quit_audio:
    av_log(NULL, AV_LOG_INFO, "audio_thread loop end\n");
    is->audio.audioq.clear();
    is->audio.audio_eof = Player::AudioInfo::audio_eof_enum::eof;
    av_log(NULL, AV_LOG_INFO, "audio_thread end\n");
}

bool Player::configure_audio_filters(AVFilterContext **filt_in, AVFilterContext **filt_out, AVFilterGraph *graph, const AudioInfo::AudioParams &audio_filter_src)
{
    char asrc_args[256] = { 0 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;

    if (!graph) return false;
    
    int ret = snprintf(asrc_args, sizeof(asrc_args),
                       "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                       audio_filter_src.freq, av_get_sample_fmt_name(audio_filter_src.fmt),
                       audio_filter_src.channels,
                       1, audio_filter_src.freq);
    if (audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%" PRIx64, audio_filter_src.channel_layout);
    
    if (avfilter_graph_create_filter(&filt_asrc,
                                     avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                     asrc_args, NULL, graph) < 0)
        return false;
    
    if (avfilter_graph_create_filter(&filt_asink,
                                     avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                     NULL, NULL, graph) < 0)
        return false;

    const enum AVSampleFormat out_sample_fmts[] = {AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE};
    if (av_opt_set_int_list(filt_asink, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN) < 0)
        return false;

    const int64_t out_channel_layouts[] = {av_get_default_channel_layout(audio.out_channels), -1};
    if (av_opt_set_int_list(filt_asink, "channel_layouts", out_channel_layouts, -1, AV_OPT_SEARCH_CHILDREN) < 0)
        return false;

    const int out_sample_rates[] = {audio.sample_rate, -1};
    if (av_opt_set_int_list(filt_asink, "sample_rates", out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN) < 0)
        return false;

    AVFilterContext *filt_last = filt_asrc;

    if (avfilter_link(filt_last, 0, filt_asink, 0) != 0)
        return false;
    
    if (avfilter_graph_config(graph, NULL) < 0)
        return false;
    
    *filt_in = filt_asrc;
    *filt_out = filt_asink;
    return true;
}

void Player::clear_soundbufer()
{
    std::unique_lock<std::mutex> lk(audio.audio_mutex);
    
    audio.read_idx = audio.write_idx = 0;
    memset(audio.audio_wav, 0, sizeof(float)*audio.buf_length*audio.out_channels);
    audio.cond_full.notify_one();
    audio.pts_base = NAN;
}

void Player::seek(int64_t pos)
{
    if(start_time_org == AV_NOPTS_VALUE)
        return;
    
    seek_pos = pos + start_time_org;
    seek_req_type = seek_type_pos;
}

void Player::seek_chapter(int inc)
{
    if(inc > 0) {
        seek_req_type = seek_type_next;
    }
    else if(inc < 0) {
        seek_req_type = seek_type_prev;
    }
}

void Player::set_pause(bool pause_state)
{
    struct stream_param *stream = (struct stream_param *)param;
    if(pause_state) {
        if(pause) return;
        
        pause = stream->sound_stop(stream->stream) != 1;
    }
    else {
        if(!pause) return;
        pause = stream->sound_play(stream->stream) != 1;

        video.frame_timer = NAN;
        video.frame_last_pts = NAN;

        master_clock_offset = NAN;
    }
}

void Player::stream_cycle_channel(int codec_type)
{
    int start_index, old_index;
    int nb_streams = pFormatCtx->nb_streams;
    AVProgram *p = NULL;
    struct stream_param *stream = (struct stream_param *)param;
    
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = old_index = video.videoStream;
    }
    else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = old_index = audio.audioStream;
    }
    else if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
        start_index = old_index = subtitle.subtitleStream;
    }
    else {
        stream->wait_stop(stream->stream);
        return;
    }

    int stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && video.videoStream != -1) {
        p = av_find_program_from_stream(pFormatCtx, NULL, video.videoStream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    while (true) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                goto found;
            }
            if (start_index == -1)
            {
                if (p) p = NULL;
                else
                    break;
            }
            stream_index = 0;
        }
        if (stream_index == start_index)
        {
            if (p) p = NULL;
            else
                break;
        }
        auto st = pFormatCtx->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->channels != 0)
                    goto found;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto found;
            default:
                break;
            }
        }
    }
    stream->wait_stop(stream->stream);
    return;
found:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];

    if (old_index == stream_index){
        stream->wait_stop(stream->stream);
        return;
    }

    av_log(NULL, AV_LOG_INFO, "Stream Change %d -> %d\n", old_index, stream_index);

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        video.videoStream = -1;
    }
    else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        audio.audioStream = -1;
    }
    else if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
        subtitle.subtitleStream = -1;
    }
    else {
        stream->wait_stop(stream->stream);
        return;
    }
    stream_component_close(old_index);
    if(stream_index >= 0)
        stream_component_open(stream_index);
    int tp;
    std::string lng;
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        video.videoStream = stream_index;
        tp = 0;
        lng = (stream_index >= 0) ? video.language : "";
    }
    else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        audio.audioStream = stream_index;
        tp = 1;
        lng = (stream_index >= 0) ? audio.language : "";
    }
    else if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
        subtitle.subtitleStream = stream_index;
        tp = 2;
        lng = (stream_index >= 0) ? subtitle.language : "";
    }
    else {
        stream->wait_stop(stream->stream);
        return;
    }

    // fix others
    if (codec_type == AVMEDIA_TYPE_VIDEO && video.videoStream != -1) {
        stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
        stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
    }
    stream->change_lang(stream->stream, lng.c_str(), tp, stream_index);
    seek((int64_t)((get_master_clock() - 3) * AV_TIME_BASE));
}

int subtitle_thread(Player *is)
{
    av_log(NULL, AV_LOG_INFO, "subtitle_thread start\n");
    AVPacket packet = { 0 };
    std::shared_ptr<SwsContext> sub_convert_ctx;
    int64_t old_serial = 0;

    is->subtitle.subpictq_active_serial = av_gettime();
    while (!is->IsQuit()) {
        AVCodecContext *subtitle_ctx = is->subtitle.subtitle_ctx.get();
        AVRational timebase = is->pFormatCtx->streams[is->subtitle.subtitleStream]->time_base;
        if (is->subtitle.subtitleq.get(&packet, 1) < 0) {
            // means we quit getting packets
            av_log(NULL, AV_LOG_INFO, "subtitle Quit\n");
            break;
        }
    retry:
        if (packet.data == flush_pkt.data) {
            av_log(NULL, AV_LOG_INFO, "subtitle buffer flush\n");
            avcodec_flush_buffers(subtitle_ctx);
            old_serial = 0;
            packet = { 0 };
            continue;
        }
        if (packet.data == eof_pkt.data) {
            av_log(NULL, AV_LOG_INFO, "subtitle buffer EOF\n");
            packet = { 0 };
            while (!is->IsQuit() && is->subtitle.subtitleq.get(&packet, 0) == 0)
                av_usleep(100*1000);
            if (is->IsQuit()) break;
            goto retry;
        }
        if (packet.data == abort_pkt.data) {
            av_log(NULL, AV_LOG_INFO, "subtitle buffer ABORT\n");
            packet = { 0 };
            break;
        }
        int got_frame = 0;
        int ret;
        AVSubtitle sub;
        if ((ret = avcodec_decode_subtitle2(subtitle_ctx, &sub, &got_frame, &packet)) < 0) {
            av_packet_unref(&packet);
            char buf[AV_ERROR_MAX_STRING_SIZE];
            char *errstr = av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
            av_log(NULL, AV_LOG_ERROR, "error avcodec_decode_subtitle2() %d %s\n", ret, errstr);
            return -1;
        }
        if(sub.pts == AV_NOPTS_VALUE)
            sub.pts = packet.pts;
        av_packet_unref(&packet);
        if (got_frame == 0) continue;

        double pts = 0;

        //printf("subtitle pts %lld\n", sub.pts);
        if (sub.pts != AV_NOPTS_VALUE)
            pts = sub.pts * av_q2d(timebase);
        std::shared_ptr<SubtitlePicture> sp(new SubtitlePicture);
        sp->pts = pts;
        sp->serial = av_gettime();
        while (old_serial >= sp->serial) sp->serial++;
        old_serial = sp->serial;
        sp->start_display_time = sub.start_display_time;
        sp->end_display_time = sub.end_display_time;
        sp->numrects = sub.num_rects;
        sp->subrects.reset(new std::shared_ptr<AVSubtitleRect>[sub.num_rects]());
        sp->type = sub.format;
        for (size_t i = 0; i < sub.num_rects; i++)
        {
            sp->subw = subtitle_ctx->width ? subtitle_ctx->width : is->video.video_ctx->width;
            sp->subh = subtitle_ctx->height ? subtitle_ctx->height : is->video.video_ctx->height;

            if (((sp->subrects[i] = std::shared_ptr<AVSubtitleRect>(
                (AVSubtitleRect *)av_mallocz(sizeof(AVSubtitleRect)),
                [](AVSubtitleRect *p) {
                if (p->text)
                    av_free(p->text);
                if (p->ass)
                    av_free(p->ass);
                if (p->data[0])
                    av_free(p->data[0]);
                av_free(p);
            })) == NULL)) {
                av_log(NULL, AV_LOG_FATAL, "Cannot allocate subtitle data\n");
                return -1;
            }

            sp->subrects[i]->type = sub.rects[i]->type;
            if (sub.rects[i]->ass)
                sp->subrects[i]->ass = av_strdup(sub.rects[i]->ass);
            if (sub.rects[i]->text)
                sp->subrects[i]->text = av_strdup(sub.rects[i]->text);
            if (sub.format == 0) {
                int width = sub.rects[i]->w * is->video.video_aspect;
                if (av_image_alloc(sp->subrects[i]->data, sp->subrects[i]->linesize, width, sub.rects[i]->h, AV_PIX_FMT_ARGB, 16) < 0) {
                    av_log(NULL, AV_LOG_FATAL, "Cannot allocate subtitle data\n");
                    return -1;
                }
                sub_convert_ctx = std::shared_ptr<SwsContext>(sws_getCachedContext(NULL,
                    sub.rects[i]->w, sub.rects[i]->h, AV_PIX_FMT_PAL8,
                    width, sub.rects[i]->h, AV_PIX_FMT_ARGB,
                    SWS_BICUBIC, NULL, NULL, NULL), &sws_freeContext);
                if (!sub_convert_ctx) {
                    av_log(NULL, AV_LOG_FATAL, "Cannot initialize the sub conversion context\n");
                    return -1;
                }
                sws_scale(sub_convert_ctx.get(),
                    sub.rects[i]->data, sub.rects[i]->linesize,
                    0, sub.rects[i]->h, sp->subrects[i]->data, sp->subrects[i]->linesize);
                sp->subrects[i]->w = width;
                sp->subrects[i]->h = sub.rects[i]->h;
                sp->subrects[i]->x = sub.rects[i]->x * is->video.video_aspect;
                sp->subrects[i]->y = sub.rects[i]->y;
            }
        }
        is->subtitle.subpictq.put(sp);
        avsubtitle_free(&sub);
    }
    av_log(NULL, AV_LOG_INFO, "subtitle thread end\n");
    return 0;
}

void Player::subtitle_display(VideoPicture *vp)
{
    struct stream_param *stream = (struct stream_param *)param;
    std::shared_ptr<SubtitlePicture> sp;
    if (subtitle.subpictq.peek(sp) == 0) {
        // skip to current present subtitle
        while (sp->serial < subtitle.subpictq_active_serial && subtitle.subpictq.get(sp) == 0)
            ;
        if (sp->serial < subtitle.subpictq_active_serial)
            goto dismiss;

        if (vp->pts > sp->pts + (double)sp->end_display_time / 1000)
            subtitle.subpictq.get(sp);
        //printf("video %f, cc %f %d %d\n", vp->pts, sp->pts, sp->start_display_time, sp->end_display_time);

        if (vp->pts <= sp->pts + (double)sp->end_display_time / 1000 &&
            vp->pts >= sp->pts + (double)sp->start_display_time / 1000) {

            if (sp->type == 0) {
                for (int i = 0; i < sp->numrects; i++) {
                    int s_w = sp->subrects[i]->w;
                    int s_h = sp->subrects[i]->h;
                    int s_x = sp->subrects[i]->x;
                    int s_y = sp->subrects[i]->y;
                    for(int y = s_y, sy = 0; y < vp->height && sy < s_h; y++, sy++){
                        uint8_t *sublp = sp->subrects[i]->data[0];
                        uint8_t *displp = vp->bmp.data[0];
                        sublp = sublp + sy * sp->subrects[i]->linesize[0];
                        displp = displp + y * vp->bmp.linesize[0];
                        for(int x = s_x, sx = 0; x < vp->width && sx < s_w; x++, sx++){
                            uint8_t *subp = &sublp[sx * 4];
                            uint8_t *disp = &displp[x * 4];
                            double a = subp[0] / 255.0;
                            uint8_t r = subp[1];
                            uint8_t g = subp[2];
                            uint8_t b = subp[3];
                            disp[0] = disp[0]*(1-a) + r*a;
                            disp[1] = disp[1]*(1-a) + g*a;
                            disp[2] = disp[2]*(1-a) + b*a;
                        }
                    }
                }
            }
            else {
                bool ass = false;
                std::ostringstream os;
                for (int i = 0; i < sp->numrects; i++) {
                    if (sp->subrects[i]->text) {
                        os << sp->subrects[i]->text << std::endl;
                    }
                    if (sp->subrects[i]->ass) {
                        os << sp->subrects[i]->ass << std::endl;
                        ass = true;
                    }
                }
                stream->cc_draw(stream->stream, os.str().c_str(), ass?1:0);
                return;
            }
        }
    }
dismiss:
    stream->cc_draw(stream->stream, NULL, 0);
}
