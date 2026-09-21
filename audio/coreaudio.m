/*
 * QEMU OS X CoreAudio audio driver
 *
 * Copyright (c) 2005 Mike Kronenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>            /* pthread_X */

#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "audio_int.h"
#include "trace.h"

#define TYPE_AUDIO_COREAUDIO "audio-coreaudio"
OBJECT_DECLARE_SIMPLE_TYPE(AudioCoreaudio, AUDIO_COREAUDIO)

struct AudioCoreaudio {
    AudioMixengBackend parent_obj;
};

typedef struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID device_id;
    int frame_size_setting;
    uint32_t buffer_count;
    UInt32 device_frame_size;
    AudioDeviceIOProcID ioprocid;
    bool enabled;
    bool running;
} CoreaudioVoiceOut;

typedef struct coreaudioVoiceIn {
    HWVoiceIn hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID device_id;
    int frame_size_setting;
    uint32_t buffer_count;
    UInt32 device_frame_size;
    AudioDeviceIOProcID ioprocid;
    bool enabled;
    bool running;
} CoreaudioVoiceIn;

/* voices of all -audiodev instances sharing one device; BQL-protected */
static struct {
    AudioDeviceID id;
    int refs;
} coreaudio_dev_users[16];

static int coreaudio_dev_refs(AudioDeviceID id)
{
    for (int i = 0; i < ARRAY_SIZE(coreaudio_dev_users); i++) {
        if (coreaudio_dev_users[i].refs && coreaudio_dev_users[i].id == id) {
            return coreaudio_dev_users[i].refs;
        }
    }
    return 0;
}

static void coreaudio_dev_ref(AudioDeviceID id)
{
    int free = -1;

    for (int i = 0; i < ARRAY_SIZE(coreaudio_dev_users); i++) {
        if (coreaudio_dev_users[i].refs && coreaudio_dev_users[i].id == id) {
            coreaudio_dev_users[i].refs++;
            return;
        }
        if (!coreaudio_dev_users[i].refs && free < 0) {
            free = i;
        }
    }
    if (free >= 0) {
        coreaudio_dev_users[free].id = id;
        coreaudio_dev_users[free].refs = 1;
    }
}

static void coreaudio_dev_unref(AudioDeviceID id)
{
    for (int i = 0; i < ARRAY_SIZE(coreaudio_dev_users); i++) {
        if (coreaudio_dev_users[i].refs && coreaudio_dev_users[i].id == id) {
            coreaudio_dev_users[i].refs--;
            return;
        }
    }
}

static bool coreaudio_adopt_format(struct audio_pcm_info *info,
                                   const AudioStreamBasicDescription *d)
{
    struct audsettings as = {
        .freq = d->mSampleRate,
        .nchannels = d->mChannelsPerFrame,
        .fmt = AUDIO_FORMAT_F32,
        .big_endian = false,
    };

    if (d->mFormatID != kAudioFormatLinearPCM ||
        !(d->mFormatFlags & kLinearPCMFormatFlagIsFloat) ||
        d->mBitsPerChannel != 32 || d->mBytesPerFrame != 4 * as.nchannels ||
        as.nchannels < 1 || as.nchannels > 2 || as.freq <= 0) {
        return false;
    }
    if (as.freq != info->freq || as.nchannels != info->nchannels) {
        audio_pcm_init_info(info, &as);
    }
    return true;
}

static const AudioObjectPropertyAddress voice_out_addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static const AudioObjectPropertyAddress voice_in_addr = {
    kAudioHardwarePropertyDefaultInputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static OSStatus coreaudio_get_voice_out(AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);

    return AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &voice_out_addr,
                                      0,
                                      NULL,
                                      &size,
                                      id);
}

static OSStatus coreaudio_get_out_framesizerange(AudioDeviceID id,
                                                 AudioValueRange *framerange)
{
    UInt32 size = sizeof(*framerange);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framerange);
}

static OSStatus coreaudio_get_out_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framesize);
}

static OSStatus coreaudio_set_out_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      size,
                                      framesize);
}

static OSStatus coreaudio_set_out_streamformat(AudioDeviceID id,
                                               AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      size,
                                      d);
}

static OSStatus coreaudio_get_out_streamformat(AudioDeviceID id,
                                               AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, d);
}

static OSStatus coreaudio_get_voice_in(AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);

    return AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &voice_in_addr,
                                      0,
                                      NULL,
                                      &size,
                                      id);
}

static OSStatus coreaudio_get_in_framesizerange(AudioDeviceID id,
                                                AudioValueRange *framerange)
{
    UInt32 size = sizeof(*framerange);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, framerange);
}

static OSStatus coreaudio_get_in_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, framesize);
}

static OSStatus coreaudio_set_in_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id, &addr, 0, NULL, size, framesize);
}

static OSStatus coreaudio_set_in_streamformat(AudioDeviceID id,
                                              AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id, &addr, 0, NULL, size, d);
}

static OSStatus coreaudio_get_in_streamformat(AudioDeviceID id,
                                              AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id, &addr, 0, NULL, &size, d);
}

static void coreaudio_logstatus(OSStatus status)
{
    const char *str = "BUG";

    switch (status) {
    case kAudioHardwareNoError:
        str = "kAudioHardwareNoError";
        break;

    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;

    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;

    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;

    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;

    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;

    case kAudioHardwareBadDeviceError:
        str = "kAudioHardwareBadDeviceError";
        break;

    case kAudioHardwareBadStreamError:
        str = "kAudioHardwareBadStreamError";
        break;

    case kAudioHardwareUnsupportedOperationError:
        str = "kAudioHardwareUnsupportedOperationError";
        break;

    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;

    case kAudioDevicePermissionsError:
        str = "kAudioDevicePermissionsError";
        break;

    default:
        error_printf(" Reason: status code %" PRId32, (int32_t)status);
        return;
    }

    error_printf(" Reason: %s", str);
}

static void G_GNUC_PRINTF(2, 3) coreaudio_logerr(OSStatus status,
                                                const char *fmt, ...)
{
    va_list ap;

    error_printf("coreaudio: ");
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    coreaudio_logstatus(status);
    error_printf("\n");
}

static void G_GNUC_PRINTF(3, 4) coreaudio_logerr2(OSStatus status,
                                                  const char *typ,
                                                  const char *fmt, ...)
{
    va_list ap;

    error_printf("coreaudio: Could not initialize %s: ", typ);
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    coreaudio_logstatus(status);
    error_printf("\n");
}

#define coreaudio_playback_logerr(status, ...) \
    coreaudio_logerr2(status, "playback", __VA_ARGS__)

#define coreaudio_capture_logerr(status, ...) \
    coreaudio_logerr2(status, "capture", __VA_ARGS__)

static int coreaudio_voice_out_buf_lock(CoreaudioVoiceOut *core,
                                        const char *fn_name)
{
    int err;

    err = pthread_mutex_lock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not lock voice for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

static int coreaudio_voice_out_buf_unlock(CoreaudioVoiceOut *core,
                                          const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not unlock voice for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args)       \
    static ret_type glue(coreaudio_, name)args_decl                   \
    {                                                                 \
        CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;            \
        ret_type ret;                                                 \
                                                                      \
        if (coreaudio_voice_out_buf_lock(core, "coreaudio_" #name)) { \
            return 0;                                                 \
        }                                                             \
                                                                      \
        ret = glue(audio_generic_, name)args;                         \
                                                                      \
        coreaudio_voice_out_buf_unlock(core, "coreaudio_" #name);     \
        return ret;                                                   \
    }
COREAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw))
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(put_buffer_out, size_t,
                       (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

/*
 * callback to feed audiooutput buffer. called without BQL.
 * allowed to lock "buf_mutex", but disallowed to have any other locks.
 */
static OSStatus out_device_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    UInt32 frame_size, pending_frames;
    void *out;
    HWVoiceOut *hw = hwptr;
    CoreaudioVoiceOut *core = hwptr;
    size_t len;

    if (!outOutputData || outOutputData->mNumberBuffers < 1) {
        return 0;
    }
    out = outOutputData->mBuffers[0].mData;

    if (coreaudio_voice_out_buf_lock(core, "out_device_ioproc")) {
        inInputTime = 0;
        return 0;
    }

    if (inDevice != core->device_id) {
        coreaudio_voice_out_buf_unlock(core, "out_device_ioproc(old device)");
        return 0;
    }

    len = outOutputData->mBuffers[0].mDataByteSize;
    frame_size = len / hw->info.bytes_per_frame;
    pending_frames = hw->pending_emul / hw->info.bytes_per_frame;
    trace_coreaudio_out_ioproc(core, inDevice, (uint32_t)len, frame_size,
                               pending_frames);

    /* if there are not enough samples, set signal and return */
    if (pending_frames < frame_size) {
        inInputTime = 0;
        coreaudio_voice_out_buf_unlock(core, "out_device_ioproc(empty)");
        return 0;
    }

    len = frame_size * hw->info.bytes_per_frame;
    while (len) {
        size_t write_len, start;

        start = audio_ring_posb(hw->pos_emul, hw->pending_emul, hw->size_emul);
        assert(start < hw->size_emul);

        write_len = MIN(MIN(hw->pending_emul, len),
                        hw->size_emul - start);

        memcpy(out, hw->buf_emul + start, write_len);
        hw->pending_emul -= write_len;
        len -= write_len;
        out += write_len;
    }

    coreaudio_voice_out_buf_unlock(core, "out_device_ioproc");
    return 0;
}

static OSStatus init_out_device(CoreaudioVoiceOut *core)
{
    AudioDeviceID device_id;
    AudioDeviceIOProcID ioprocid;
    AudioValueRange value_range;
    OSStatus status;
    UInt32 device_frame_size;
    bool first;

    AudioStreamBasicDescription stream_basic_description = {
        .mBitsPerChannel = audio_format_bits(core->hw.info.af),
        .mBytesPerFrame = core->hw.info.bytes_per_frame,
        .mBytesPerPacket = core->hw.info.bytes_per_frame,
        .mChannelsPerFrame = core->hw.info.nchannels,
        .mFormatFlags = kLinearPCMFormatFlagIsFloat,
        .mFormatID = kAudioFormatLinearPCM,
        .mFramesPerPacket = 1,
        .mSampleRate = core->hw.info.freq
    };

    status = coreaudio_get_voice_out(&device_id);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get default output device");
        return status;
    }
    if (device_id == kAudioDeviceUnknown) {
        error_report("coreaudio: Could not initialize playback: "
                     "Unknown audio device");
        return status;
    }

    first = coreaudio_dev_refs(device_id) == 0;

    /* get minimum and maximum buffer frame sizes */
    status = coreaudio_get_out_framesizerange(device_id, &value_range);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get device buffer frame range");
        return status;
    }

    if (value_range.mMinimum > core->frame_size_setting) {
        device_frame_size = value_range.mMinimum;
        warn_report("coreaudio: Upsizing buffer frames to %f",
                    value_range.mMinimum);
    } else if (value_range.mMaximum < core->frame_size_setting) {
        device_frame_size = value_range.mMaximum;
        warn_report("coreaudio: Downsizing buffer frames to %f",
                    value_range.mMaximum);
    } else {
        device_frame_size = core->frame_size_setting;
    }

    /* set Buffer Frame Size */
    if (first) {
        status = coreaudio_set_out_framesize(device_id, &device_frame_size);
        if (status == kAudioHardwareBadObjectError) {
            return 0;
        }
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not set device buffer frame size %" PRIu32,
                                      (uint32_t)device_frame_size);
            return status;
        }
    }

    /* get Buffer Frame Size */
    status = coreaudio_get_out_framesize(device_id, &device_frame_size);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get device buffer frame size");
        return status;
    }

    /* set Samplerate */
    if (first) {
        status = coreaudio_set_out_streamformat(device_id,
                                                &stream_basic_description);
        if (status == kAudioHardwareBadObjectError) {
            return 0;
        }
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not set samplerate %lf",
                                      stream_basic_description.mSampleRate);
            return status;
        }
    }

    /* get stream format */
    status = coreaudio_get_out_streamformat(device_id,
                                            &stream_basic_description);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status, "Could not get stream format");
        return status;
    }
    if (!coreaudio_adopt_format(&core->hw.info, &stream_basic_description)) {
        error_report("coreaudio: Could not initialize playback: "
                     "unsupported stream format %u ch %u bit flags 0x%x",
                     (unsigned)stream_basic_description.mChannelsPerFrame,
                     (unsigned)stream_basic_description.mBitsPerChannel,
                     (unsigned)stream_basic_description.mFormatFlags);
        return -1;
    }
    trace_coreaudio_init_out(core, device_id, core->frame_size_setting,
                             device_frame_size,
                             (uint32_t)stream_basic_description.mSampleRate,
                             stream_basic_description.mChannelsPerFrame,
                             stream_basic_description.mFormatFlags,
                             stream_basic_description.mBitsPerChannel,
                             stream_basic_description.mBytesPerFrame);

    /*
     * set Callback.
     *
     * On macOS 11.3.1, Core Audio calls AudioDeviceIOProc after calling an
     * internal function named HALB_Mutex::Lock(), which locks a mutex in
     * HALB_IOThread::Entry(void*). HALB_Mutex::Lock() is also called in
     * AudioObjectGetPropertyData, which is called by coreaudio driver.
     * Therefore, the specified callback must be designed to avoid a deadlock
     * with the callers of AudioObjectGetPropertyData.
     */
    ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(device_id,
                                       out_device_ioproc,
                                       &core->hw,
                                       &ioprocid);
    if (status == kAudioHardwareBadDeviceError) {
        return 0;
    }
    if (status != kAudioHardwareNoError || ioprocid == NULL) {
        coreaudio_playback_logerr(status, "Could not set IOProc");
        return status;
    }

    core->device_id = device_id;
    core->device_frame_size = device_frame_size;
    core->hw.samples = core->buffer_count * core->device_frame_size;
    audio_generic_initialize_buffer_out(&core->hw);
    core->ioprocid = ioprocid;
    coreaudio_dev_ref(device_id);

    return 0;
}

static void fini_out_device(CoreaudioVoiceOut *core)
{
    OSStatus status;

    if (core->device_id == kAudioDeviceUnknown) {
        return;
    }

    /* stop playback */
    if (core->running) {
        status = AudioDeviceStop(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not stop playback");
        }
        core->running = false;
    }

    /* remove callback */
    status = AudioDeviceDestroyIOProcID(core->device_id,
                                        core->ioprocid);
    if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove IOProc");
    }
    coreaudio_dev_unref(core->device_id);
    core->device_id = kAudioDeviceUnknown;
}

/* kAudioDevicePropertyDeviceIsRunning is device-wide; track this IOProc */
static void update_out_device_playback_state(CoreaudioVoiceOut *core)
{
    OSStatus status;

    if (core->device_id == kAudioDeviceUnknown) {
        return;
    }

    if (core->enabled && !core->running) {
        status = AudioDeviceStart(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not resume playback");
        }
        core->running = status == kAudioHardwareNoError;
    } else if (!core->enabled && core->running) {
        status = AudioDeviceStop(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not pause playback");
        }
        core->running = false;
    }
}

/* called without BQL. */
static OSStatus handle_voice_out_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    CoreaudioVoiceOut *core = in_client_data;

    bql_lock();

    if (core->device_id) {
        fini_out_device(core);
    }

    init_out_device(core);

    if (core->device_id) {
        update_out_device_playback_state(core);
    }

    bql_unlock();
    return 0;
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    OSStatus status;
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;
    int err;
    Audiodev *dev = hw->s->dev;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.out;
    struct audsettings obt_as;

    /* create mutex */
    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        error_report("coreaudio: Could not create mutex: %s", strerror(err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info(&hw->info, as);

    core->frame_size_setting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);

    core->buffer_count = cpdo->has_buffer_count ? cpdo->buffer_count : 4;

    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            &voice_out_addr,
                                            handle_voice_out_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not listen to voice property change");
        return -1;
    }

    if (init_out_device(core)) {
        status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                                   &voice_out_addr,
                                                   handle_voice_out_change,
                                                   core);
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not remove voice property change listener");
        }

        return -1;
    }

    return 0;
}

static void coreaudio_fini_out (HWVoiceOut *hw)
{
    OSStatus status;
    int err;
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;

    status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                               &voice_out_addr,
                                               handle_voice_out_change,
                                               core);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove voice property change listener");
    }

    fini_out_device(core);

    /* destroy mutex */
    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not destroy mutex: %s", strerror(err));
    }
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;

    core->enabled = enable;
    update_out_device_playback_state(core);
}

/* ------------------------------------------------------------------ */
/* Audio input (capture)                                              */
/* ------------------------------------------------------------------ */

static int coreaudio_voice_in_buf_lock(CoreaudioVoiceIn *core,
                                       const char *fn_name)
{
    int err;

    err = pthread_mutex_lock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not lock voice in for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

static int coreaudio_voice_in_buf_unlock(CoreaudioVoiceIn *core,
                                         const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not unlock voice in for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

#define COREAUDIO_WRAPPER_FUNC_IN(name, ret_type, args_decl, args)       \
    static ret_type glue(coreaudio_, name)args_decl                      \
    {                                                                    \
        CoreaudioVoiceIn *core = (CoreaudioVoiceIn *)hw;                 \
        ret_type ret;                                                    \
                                                                         \
        if (coreaudio_voice_in_buf_lock(core, "coreaudio_" #name)) {     \
            return 0;                                                    \
        }                                                                \
                                                                         \
        ret = glue(audio_generic_, name)args;                            \
                                                                         \
        coreaudio_voice_in_buf_unlock(core, "coreaudio_" #name);         \
        return ret;                                                      \
    }
COREAUDIO_WRAPPER_FUNC_IN(get_buffer_in, void *, (HWVoiceIn *hw, size_t *size),
                           (hw, size))
COREAUDIO_WRAPPER_FUNC_IN(read, size_t, (HWVoiceIn *hw, void *buf, size_t size),
                           (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC_IN

static void coreaudio_put_buffer_in(HWVoiceIn *hw, void *buf, size_t size)
{
    CoreaudioVoiceIn *core = (CoreaudioVoiceIn *)hw;

    if (coreaudio_voice_in_buf_lock(core, "coreaudio_put_buffer_in")) {
        return;
    }

    audio_generic_put_buffer_in(hw, buf, size);

    coreaudio_voice_in_buf_unlock(core, "coreaudio_put_buffer_in");
}

/*
 * Callback to receive audio input. Called without BQL.
 * Allowed to lock "buf_mutex", but disallowed to have any other locks.
 */
static OSStatus in_device_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    HWVoiceIn *hw = hwptr;
    CoreaudioVoiceIn *core = hwptr;
    void *in;
    UInt32 frame_size;
    size_t len;

    if (!inInputData || inInputData->mNumberBuffers < 1) {
        return 0;
    }

    in = inInputData->mBuffers[0].mData;

    if (coreaudio_voice_in_buf_lock(core, "in_device_ioproc")) {
        return 0;
    }

    if (inDevice != core->device_id) {
        coreaudio_voice_in_buf_unlock(core, "in_device_ioproc(old device)");
        return 0;
    }

    len = inInputData->mBuffers[0].mDataByteSize;
    frame_size = len / hw->info.bytes_per_frame;
    len = frame_size * hw->info.bytes_per_frame;
    trace_coreaudio_in_ioproc(core, inDevice, (uint32_t)len, frame_size);

    while (len) {
        size_t write_len;
        size_t free = hw->size_emul - hw->pending_emul;

        if (free == 0) {
            break; /* buffer full, drop remaining */
        }

        write_len = MIN(MIN(free, len), hw->size_emul - hw->pos_emul);
        memcpy(hw->buf_emul + hw->pos_emul, in, write_len);
        hw->pos_emul = (hw->pos_emul + write_len) % hw->size_emul;
        hw->pending_emul += write_len;
        len -= write_len;
        in += write_len;
    }

    coreaudio_voice_in_buf_unlock(core, "in_device_ioproc");
    return 0;
}

static OSStatus init_in_device(CoreaudioVoiceIn *core)
{
    AudioDeviceID device_id;
    AudioDeviceIOProcID ioprocid;
    AudioValueRange value_range;
    OSStatus status;
    UInt32 device_frame_size;
    bool first;

    AudioStreamBasicDescription stream_basic_description = {
        .mBitsPerChannel = audio_format_bits(core->hw.info.af),
        .mBytesPerFrame = core->hw.info.bytes_per_frame,
        .mBytesPerPacket = core->hw.info.bytes_per_frame,
        .mChannelsPerFrame = core->hw.info.nchannels,
        .mFormatFlags = kLinearPCMFormatFlagIsFloat,
        .mFormatID = kAudioFormatLinearPCM,
        .mFramesPerPacket = 1,
        .mSampleRate = core->hw.info.freq
    };

    status = coreaudio_get_voice_in(&device_id);
    if (status != kAudioHardwareNoError) {
        coreaudio_capture_logerr(status,
                                 "Could not get default input device");
        return status;
    }
    if (device_id == kAudioDeviceUnknown) {
        error_report("coreaudio: Could not initialize capture: "
                     "Unknown audio input device");
        return -1;
    }

    first = coreaudio_dev_refs(device_id) == 0;

    /* get minimum and maximum buffer frame sizes */
    status = coreaudio_get_in_framesizerange(device_id, &value_range);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_capture_logerr(status,
                                 "Could not get input device buffer frame range");
        return status;
    }

    if (value_range.mMinimum > core->frame_size_setting) {
        device_frame_size = value_range.mMinimum;
    } else if (value_range.mMaximum < core->frame_size_setting) {
        device_frame_size = value_range.mMaximum;
    } else {
        device_frame_size = core->frame_size_setting;
    }

    /* set Buffer Frame Size */
    if (first) {
        status = coreaudio_set_in_framesize(device_id, &device_frame_size);
        if (status == kAudioHardwareBadObjectError) {
            return 0;
        }
        if (status != kAudioHardwareNoError) {
            coreaudio_capture_logerr(status,
                                     "Could not set input device buffer frame size %" PRIu32,
                                     (uint32_t)device_frame_size);
            return status;
        }
    }

    /* get Buffer Frame Size */
    status = coreaudio_get_in_framesize(device_id, &device_frame_size);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_capture_logerr(status,
                                 "Could not get input device buffer frame size");
        return status;
    }

    /* set stream format */
    if (first) {
        status = coreaudio_set_in_streamformat(device_id,
                                               &stream_basic_description);
        if (status == kAudioHardwareBadObjectError) {
            return 0;
        }
        if (status != kAudioHardwareNoError) {
            coreaudio_capture_logerr(status,
                                     "Could not set input samplerate %lf",
                                     stream_basic_description.mSampleRate);
            return status;
        }
    }

    /* get stream format */
    status = coreaudio_get_in_streamformat(device_id,
                                           &stream_basic_description);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_capture_logerr(status, "Could not get input stream format");
        return status;
    }
    if (!coreaudio_adopt_format(&core->hw.info, &stream_basic_description)) {
        error_report("coreaudio: Could not initialize capture: "
                     "unsupported stream format %u ch %u bit flags 0x%x",
                     (unsigned)stream_basic_description.mChannelsPerFrame,
                     (unsigned)stream_basic_description.mBitsPerChannel,
                     (unsigned)stream_basic_description.mFormatFlags);
        return -1;
    }
    trace_coreaudio_init_in(core, device_id, core->frame_size_setting,
                            device_frame_size,
                            (uint32_t)stream_basic_description.mSampleRate,
                            stream_basic_description.mChannelsPerFrame,
                            stream_basic_description.mFormatFlags,
                            stream_basic_description.mBitsPerChannel,
                            stream_basic_description.mBytesPerFrame);

    /* set Callback */
    ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(device_id,
                                       in_device_ioproc,
                                       &core->hw,
                                       &ioprocid);
    if (status == kAudioHardwareBadDeviceError) {
        return 0;
    }
    if (status != kAudioHardwareNoError || ioprocid == NULL) {
        coreaudio_capture_logerr(status, "Could not set input IOProc");
        return status;
    }

    core->device_id = device_id;
    core->device_frame_size = device_frame_size;
    core->hw.samples = core->buffer_count * core->device_frame_size;
    audio_generic_initialize_buffer_in(&core->hw);
    core->ioprocid = ioprocid;
    coreaudio_dev_ref(device_id);

    return 0;
}

static void fini_in_device(CoreaudioVoiceIn *core)
{
    OSStatus status;

    if (core->device_id == kAudioDeviceUnknown) {
        return;
    }

    if (core->running) {
        status = AudioDeviceStop(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError &&
            status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not stop capture");
        }
        core->running = false;
    }

    status = AudioDeviceDestroyIOProcID(core->device_id, core->ioprocid);
    if (status != kAudioHardwareBadDeviceError &&
        status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove input IOProc");
    }
    coreaudio_dev_unref(core->device_id);
    core->device_id = kAudioDeviceUnknown;
}

static void update_in_device_capture_state(CoreaudioVoiceIn *core)
{
    OSStatus status;

    if (core->device_id == kAudioDeviceUnknown) {
        return;
    }

    if (core->enabled && !core->running) {
        status = AudioDeviceStart(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError &&
            status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not resume capture");
        }
        core->running = status == kAudioHardwareNoError;
    } else if (!core->enabled && core->running) {
        status = AudioDeviceStop(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError &&
            status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not pause capture");
        }
        core->running = false;
    }
}

/* called without BQL. */
static OSStatus handle_voice_in_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    CoreaudioVoiceIn *core = in_client_data;

    bql_lock();

    if (core->device_id) {
        fini_in_device(core);
    }

    init_in_device(core);

    if (core->device_id) {
        update_in_device_capture_state(core);
    }

    bql_unlock();
    return 0;
}

static int coreaudio_init_in(HWVoiceIn *hw, struct audsettings *as)
{
    OSStatus status;
    CoreaudioVoiceIn *core = (CoreaudioVoiceIn *)hw;
    int err;
    Audiodev *dev = hw->s->dev;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.in;
    struct audsettings obt_as;

    /* create mutex */
    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        error_report("coreaudio: Could not create mutex: %s", strerror(err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info(&hw->info, as);

    core->frame_size_setting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);

    core->buffer_count = cpdo->has_buffer_count ? cpdo->buffer_count : 4;

    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            &voice_in_addr,
                                            handle_voice_in_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        coreaudio_capture_logerr(status,
                                 "Could not listen to input voice property change");
        return -1;
    }

    if (init_in_device(core)) {
        status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                                   &voice_in_addr,
                                                   handle_voice_in_change,
                                                   core);
        if (status != kAudioHardwareNoError) {
            coreaudio_capture_logerr(status,
                                     "Could not remove input voice property change listener");
        }
        return -1;
    }

    return 0;
}

static void coreaudio_fini_in(HWVoiceIn *hw)
{
    OSStatus status;
    int err;
    CoreaudioVoiceIn *core = (CoreaudioVoiceIn *)hw;

    status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                               &voice_in_addr,
                                               handle_voice_in_change,
                                               core);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status,
                         "Could not remove input voice property change listener");
    }

    fini_in_device(core);

    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not destroy mutex: %s", strerror(err));
    }
}

static void coreaudio_enable_in(HWVoiceIn *hw, bool enable)
{
    CoreaudioVoiceIn *core = (CoreaudioVoiceIn *)hw;

    core->enabled = enable;
    update_in_device_capture_state(core);
}

static void audio_coreaudio_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 1;
    k->voice_size_out = sizeof(CoreaudioVoiceOut);
    k->voice_size_in = sizeof(CoreaudioVoiceIn);

    k->init_out = coreaudio_init_out;
    k->fini_out = coreaudio_fini_out;
    /* wrapper for audio_generic_write */
    k->write = coreaudio_write;
    /* wrapper for audio_generic_buffer_get_free */
    k->buffer_get_free = coreaudio_buffer_get_free;
    /* wrapper for audio_generic_get_buffer_out */
    k->get_buffer_out = coreaudio_get_buffer_out;
    /* wrapper for audio_generic_put_buffer_out */
    k->put_buffer_out = coreaudio_put_buffer_out;
    k->enable_out = coreaudio_enable_out;

    k->init_in = coreaudio_init_in;
    k->fini_in = coreaudio_fini_in;
    k->read = coreaudio_read;
    k->get_buffer_in = coreaudio_get_buffer_in;
    k->put_buffer_in = coreaudio_put_buffer_in;
    k->enable_in = coreaudio_enable_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_COREAUDIO,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioCoreaudio),
        .class_init = audio_coreaudio_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_COREAUDIO);
