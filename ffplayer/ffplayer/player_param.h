//
//  player_param.h
//  fftest
//
//  Created by rei8 on 2019/10/18.
//  Copyright © 2019 lithium03. All rights reserved.
//

#ifndef player_param_h
#define player_param_h

struct stream_param {
    char *name;
    double latency;
    double start_skip;
    double play_duration;
    int media_count;
    void *stream;
    void *player;
    int(*read_packet)(void *opaque, unsigned char *buf, int buf_size);
    long long(*seek)(void *opaque, long long offset, int whence);
    void(*cancel)(void *opaque);
    int(*get_width)(void *opaque);
    int(*get_height)(void *opaque);
    void(*draw_pict)(void *opaque, unsigned char *image, int width, int height, int linesize, double t);
    void(*set_duration)(void *opaque, double duration);
    void(*set_soundonly)(void *opaque);
    int(*sound_play)(void *opaque);
    int(*sound_stop)(void *opaque);
    void(*wait_stop)(void *opaque);
    void(*wait_start)(void *opaque);
    void(*cc_draw)(void *opaque, const char *buffer, int type);
    void(*change_lang)(void *opaque, const char *buffer, int type, int idx);
};

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

void setParam(struct stream_param * param);
void freeParam(struct stream_param * param);
void quitPlayer(struct stream_param * param);

void seekPlayer(struct stream_param * param, long long pos);
void seekPlayerChapter(struct stream_param * param, int inc);
void cycleChancelPlayer(struct stream_param * param, int type);
void pausePlayer(struct stream_param * param, int state);
int getPlayer_pause(struct stream_param * param);

int createParseThread(struct stream_param * param);
int waitParseThread(struct stream_param * param);

#ifdef __cplusplus
}
#endif /* __cplusplus */
    
#define VIDEO_PICTURE_QUEUE_SIZE 50

#define MAX_AUDIOQ_SIZE (1 * 1024 * 1024)
#define MAX_VIDEOQ_SIZE (16 * 1024 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 9.0
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1

#endif /* player_param_h */
