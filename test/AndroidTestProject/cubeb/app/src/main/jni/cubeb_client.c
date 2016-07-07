//
// Created by Alex Chronopoulos on 5/20/16.
//

#include "cubeb_client.h"
#include "cubeb/cubeb.h"
#include <android/log.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "hello-jni-utils.h"

#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "cubeb_client" , ## args)

//#define SAMPLE_FREQUENCY 48000
//#define SAMPLE_FREQUENCY 44100
#define SAMPLE_FREQUENCY 35000
//#define SAMPLE_FREQUENCY 47000

#define CHANNELS_NUM 1
#define CHANNELS_INPUT 1

//#define TEST_DRAIN

cubeb * ctx = NULL;
cubeb_stream * stream=NULL;

typedef struct  {
    long position;
    cubeb_stream ** p;
    enum mode operation_mode;
} user_data;
user_data data={0};

int init_client()
{
    if (ctx) {
        LOG("Client already exist");
        return CUBEB_OK;
    }
    int ret = cubeb_init(&ctx, "Example for Android");
    if (ret != CUBEB_OK) {
        LOG("Client init failed with error code %d", ret);
        return ret;
    }

    LOG("Client init success");
    return ret;
}

void state_cb(cubeb_stream * stm, void * user, cubeb_state state)
{
    LOG("state=%d\n", state);
}

long data_cb(cubeb_stream * stm, void * user, const void * input_buffer, void * output_buffer, long nframes)
{
    user_data * u = user;
    assert(u == &data);
    LOG("user callback nframe = %ld", nframes);

    if ( u->operation_mode == READ_WRITE ) {
        // On full duplex echo the input
        memcpy(output_buffer, input_buffer, CHANNELS_INPUT*sizeof(uint16_t)*nframes);
    } else if (u->operation_mode == WRITE) {
        // On out only play a tone
        short * buf = (short*)output_buffer;
        for (short int i = 0; i < nframes; ++i) {
            for (short int c = 0; c < CHANNELS_NUM; ++c) {
                buf[i*CHANNELS_NUM + c] = (short)16000*sin(2*M_PI*(i + u->position)*350/SAMPLE_FREQUENCY);
                buf[i*CHANNELS_NUM + c] += (short)16000*sin(2*M_PI*(i + u->position)*440/SAMPLE_FREQUENCY);
            }
        }
    } else if (u->operation_mode == READ) {
        // On input only store it in file
        append_on_file("/sdcard/cubeb_test_file.s16", input_buffer, CHANNELS_INPUT*sizeof(uint16_t)*nframes);
    }

    u->position += nframes;

#ifdef TEST_DRAIN
    if (u->position > 100000) {
        // test drain
        LOG("drain");
        return nframes / 2;
//        return 0;
//        return -1;
//        return -10;
    }
#endif

    return nframes;
}

int init_stream(enum mode m)
{
    if (stream) {
        LOG("Stream already exist in mode: %d", data.operation_mode);
        return CUBEB_OK;
    }
    int ret =  CUBEB_ERROR;
    unsigned int latency_ms = 20;

    cubeb_stream_params* p_in_params = NULL;
    cubeb_stream_params* p_out_params = NULL;

    cubeb_stream_params params;
    unsigned int * p_out_devid = NULL;
    if (m == WRITE || m == READ_WRITE) {
        params.format = CUBEB_SAMPLE_S16NE;
        params.rate = SAMPLE_FREQUENCY;
        params.channels = CHANNELS_NUM;
        p_out_params = &params;
    }

    cubeb_stream_params input_params;
    unsigned int * p_in_devid = NULL;
    if (m == READ || m == READ_WRITE) {
        input_params.format = CUBEB_SAMPLE_S16NE;
        input_params.rate = SAMPLE_FREQUENCY;
        input_params.channels = CHANNELS_INPUT;
        p_in_params = &input_params;
        remove_file("/sdcard/cubeb_test_file.s16");
    }

    data.p = &stream;
    data.operation_mode = m;
    ret = cubeb_stream_init(ctx,
                            &stream,
                            "Example Stream 1",
                            p_in_devid,
                            p_in_params,
                            p_out_devid,
                            p_out_params,
                            latency_ms,
                            data_cb,
                            state_cb,
                            &data);
    if (ret != CUBEB_OK) {
        LOG("Stream init failed with error code %d", ret);
        return ret;
    }

    LOG("Stream init success");
    return ret;
}

int start_stream()
{
    assert(stream);
    int ret = cubeb_stream_start(stream);
    if (ret != CUBEB_OK){
        LOG("Stream start failed with error code %d", ret);
        return ret;
    }
    LOG("Stream start success");
    return ret;
}

int stop_stream()
{
    if (!stream) {
        LOG("Stream already destroyed");
        return CUBEB_OK;
    }
    assert(stream);
    int ret = cubeb_stream_stop(stream);
    if (ret != CUBEB_OK){
        LOG("Stream stop failed with error code %d", ret);
        return ret;
    }
    memset(&data, 0, sizeof(user_data));
    LOG("Stream stop success");
    return ret;
}

int destroy_stream()
{
    if (!stream) {
        LOG("Stream already stopped");
        return CUBEB_OK;
    }
    assert(stream);
    cubeb_stream_destroy(stream);
    stream = NULL;
    LOG("Stream destroyed");
    return CUBEB_OK;
}

int destroy_client()
{
    if (!ctx) {
        LOG("Client already destroyed");
        return CUBEB_OK;
    }
    assert(ctx);
    cubeb_destroy(ctx);
    ctx = NULL;
    LOG("Client destroyed");
    return CUBEB_OK;
}
