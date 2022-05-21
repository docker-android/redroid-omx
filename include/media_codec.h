#pragma once

extern "C" {

    enum codec_type_t {
        H264_ENCODE,
        H264_DECODE,
    };

    struct media_codec_t {

        void *(*codec_alloc)(codec_type_t type, char const *node);

        int (*encode_frame)(void *context, void *buffer_handle, void *out_buf, int *out_size);

        int (*request_key_frame)(void *context);

        int (*codec_free)(void *context);
    };

}
