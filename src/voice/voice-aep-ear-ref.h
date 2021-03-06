/*
 * Copyright (C) 2010 Nokia Corporation.
 *
 * Contact: Maemo MMF Audio <mmf-audio@projects.maemo.org>
 *          or Jyri Sarha <jyri.sarha@nokia.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */
#ifndef voice_aep_ear_ref_h
#define voice_aep_ear_ref_h

#include <pulsecore/core-rtclock.h>
#include <pulse/timeval.h>
#include "memory.h"
#include "voice-util.h"

#include "module-voice-userdata.h"

/* TODO: Split into separate .c file */

/* FIXME: These shared constants should be defined in some header file in the
 * main pulseaudio tree. */
#define PA_SINK_MESSAGE_GET_UNDERRUN    (PA_SINK_MESSAGE_MAX + 50)
#define PA_SOURCE_MESSAGE_GET_OVERRUN   (PA_SOURCE_MESSAGE_MAX + 1)

enum { VOICE_EAR_REF_RESET = 0,
       VOICE_EAR_REF_UL_READY,
       VOICE_EAR_REF_DL_READY,
       VOICE_EAR_REF_RUNNING,
       VOICE_EAR_REF_QUIT };

static inline
void voice_aep_ear_ref_loop_reset(struct userdata *u) {
    struct voice_aep_ear_ref *r = &u->ear_ref;
    pa_log_debug("Ear ref loop reset initiated");
    pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
}

static inline
void voice_aep_ear_ref_init(struct userdata *u) {
    pa_assert(u);
    struct voice_aep_ear_ref *r = &u->ear_ref;
    /* NOTE: 0 (zero) value would make more sense here. However, because
       everything was tuned with value corresponding to -3333 I do not
       want to change that in Frematle (not before S3 anyway).
    */
    r->loop_padding_usec = -3333; /* The default value (usec)*/
    pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
    VOICE_TIMEVAL_INVALIDATE(&r->loop_tstamp);
    r->loop_asyncq = pa_asyncq_new(16);
    pa_assert(r->loop_asyncq);
    r->loop_memblockq = pa_memblockq_new((int64_t)0, 20*u->aep_fragment_size, /* = 200ms */
                            0, pa_frame_size(&u->aep_sample_spec), 0, 0, 0, NULL);
    pa_assert(r->loop_memblockq);
}

static inline
void voice_aep_ear_ref_unload(struct userdata *u) {
    pa_assert(u);
    struct voice_aep_ear_ref *r = &u->ear_ref;
    pa_atomic_store(&r->loop_state, VOICE_EAR_REF_QUIT);
    pa_memchunk *chunk;
    while((chunk = pa_asyncq_pop(r->loop_asyncq, 0))) {
        voice_memchunk_pool_free(u, chunk);
    }
    pa_asyncq_free(r->loop_asyncq, NULL);
    VOICE_TIMEVAL_INVALIDATE(&r->loop_tstamp);
    pa_memblockq_free(r->loop_memblockq);
    r->loop_memblockq = NULL;
}

static inline
int voice_aep_ear_ref_check_dl_xrun(struct userdata *u) {
    struct voice_aep_ear_ref *r = &u->ear_ref;
    pa_bool_t underrun;

    if (u->master_sink) {
        PA_MSGOBJECT(u->master_sink)->process_msg(
                PA_MSGOBJECT(u->master_sink), PA_SINK_MESSAGE_GET_UNDERRUN, &underrun, (int64_t)0, NULL);
    }

    if (underrun) {
        pa_log_debug("DL XRUN -> reset");
        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
        return 1;
    }

    return 0;
}

static inline
int voice_aep_ear_ref_check_ul_xrun(struct userdata *u) {
    struct voice_aep_ear_ref *r = &u->ear_ref;
    pa_bool_t overrun;

    if (u->master_source) {
        PA_MSGOBJECT(u->master_source)->process_msg(
                PA_MSGOBJECT(u->master_source), PA_SOURCE_MESSAGE_GET_OVERRUN, &overrun, (int64_t)0, NULL);
    }

    if (overrun) {
        pa_log_debug("UL XRUN -> reset");
        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
        return 1;
    }

    return 0;
}

static inline
int voice_aep_ear_ref_dl_push_to_syncq(struct userdata *u, pa_memchunk *chunk) {
    pa_memchunk *qchunk = voice_memchunk_pool_get(u);
    if (qchunk == NULL)
        return -1;
    *qchunk = *chunk;
    pa_memblock_ref(qchunk->memblock);
    static int fail_count = 0;
    if (pa_asyncq_push(u->ear_ref.loop_asyncq, qchunk, FALSE)) {
        pa_memblock_unref(qchunk->memblock);
        voice_memchunk_pool_free(u, qchunk);
        if (fail_count == 0)
            pa_log_debug("Failed to push dl frame to asyncq");
        fail_count++;
    }
    else if (fail_count > 0) {
        if (fail_count > 1)
            pa_log_debug("Failed to push dl frame to asyncq %d times", fail_count);
        fail_count = 0;
    }

    return 0;
}

static inline
int voice_aep_ear_ref_dl(struct userdata *u, pa_memchunk *chunk) {
    pa_assert(u);
    struct voice_aep_ear_ref *r = &u->ear_ref;

    int loop_state = pa_atomic_load(&r->loop_state);
    switch (loop_state) {
    case  VOICE_EAR_REF_DL_READY:
        pa_log_warn("EAR REF: consecutive DL in reset sequence -> re-reset");
        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
        break;
    case  VOICE_EAR_REF_RUNNING: {
        if (!voice_aep_ear_ref_check_dl_xrun(u)) {
            if (voice_aep_ear_ref_dl_push_to_syncq(u, chunk))
                return -1;
        }
        break;
    }
    case VOICE_EAR_REF_UL_READY: {
        struct timeval tv;
        pa_usec_t latency, si_rendered;

        PA_MSGOBJECT(u->master_sink)->process_msg(
            PA_MSGOBJECT(u->master_sink), PA_SINK_MESSAGE_GET_LATENCY, &latency, (int64_t)0, NULL);

        si_rendered = pa_bytes_to_usec((uint64_t)pa_memblockq_get_length(u->hw_sink_input->thread_info.render_memblockq),
            &u->master_sink->sample_spec);

        pa_rtclock_get(&tv);
        pa_timeval_add(&tv, latency);
        pa_timeval_add(&tv, si_rendered);
        r->loop_tstamp = tv;
        pa_log_debug("Ear ref loop DL due at %d.%06d (%lld latency) (%lld si rendered)",
                    (int)tv.tv_sec, (int)tv.tv_usec, latency,
                    si_rendered);

        if (voice_aep_ear_ref_dl_push_to_syncq(u, chunk))
            return -1;

        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_DL_READY);
        break;
    }
    case VOICE_EAR_REF_QUIT:
    case VOICE_EAR_REF_RESET: {
            /* Reset sequence starts on UL side, nothing to do. */
            break;
        }
    }
    return 0;
}

static inline
int voice_aep_ear_ref_ul_drain_asyncq(struct userdata *u, pa_bool_t push_forward) {
    struct voice_aep_ear_ref *r = &u->ear_ref;
    pa_memchunk *chunk;
    int queue_counter = 0;
    while ((chunk = pa_asyncq_pop(r->loop_asyncq, FALSE))) {
        queue_counter++;
        if (push_forward) {
            if (pa_memblockq_push(r->loop_memblockq, chunk) < 0) {
                pa_log_debug("Failed to push %d bytes of ear ref data to loop_memblockq (len %d max %d )",
                             chunk->length, pa_memblockq_get_length(r->loop_memblockq),
                             pa_memblockq_get_maxlength(r->loop_memblockq));
                voice_aep_ear_ref_loop_reset(u);
            }
        }
        pa_memblock_unref(chunk->memblock);
        voice_memchunk_pool_free(u, chunk);
    }
    return queue_counter;
}

static inline
int voice_aep_ear_ref_ul(struct userdata *u, pa_memchunk *chunk) {
    pa_assert(u);
    struct voice_aep_ear_ref *r = &u->ear_ref;
    int ret = 0;
    while (ret == 0) {
        int loop_state = pa_atomic_load(&r->loop_state);
        switch (loop_state) {
            case VOICE_EAR_REF_RUNNING: {
                if (!voice_aep_ear_ref_check_ul_xrun(u)) {
                    voice_aep_ear_ref_ul_drain_asyncq(u, TRUE);
                    if (util_memblockq_to_chunk(u->core->mempool, r->loop_memblockq, chunk, u->aep_fragment_size)) {
                        ret = 1;
                    }
                    else {
                        /* Queue has run out, reset the queue. */
                        pa_log_debug("Only %d bytes left in ear ref loop, let's reset the loop",
                                     pa_memblockq_get_length(r->loop_memblockq));
                        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
                    }
                }
            }
            break;
            case VOICE_EAR_REF_RESET: {
                voice_aep_ear_ref_ul_drain_asyncq(u, FALSE);
                pa_memblockq_drop(r->loop_memblockq, pa_memblockq_get_length(r->loop_memblockq));
                pa_atomic_store(&r->loop_state, VOICE_EAR_REF_UL_READY);
            }
            break;
            case VOICE_EAR_REF_DL_READY: {
                struct timeval tv_ul_tstamp;
                struct timeval tv_dl_tstamp = r->loop_tstamp;
                pa_assert(VOICE_TIMEVAL_IS_VALID(&tv_dl_tstamp));
                pa_usec_t latency;
                PA_MSGOBJECT(u->master_source)->process_msg(
                PA_MSGOBJECT(u->master_source), PA_SOURCE_MESSAGE_GET_LATENCY, &latency, (int64_t)0, NULL);
                /* HACK to fix AEC in VoIP

                  This hack is needed because cellular call and VoIP calls use different hw buffer sizes.
                  Currently cellular call uses 10ms and VoIP calls uses 5ms hw buffer size.

                  If the fix is made here, a correct fix for this could be like this:
                  latency -= aep_fragment_usec - hw_buffer_usec;

                  Currently is not possible to get the hw buffer size from alsa-source-old.
                  It's a possibility to add PA_SOURCE_MESSAGE_GET_HW_BUFFER_SIZE message but
                  that makes voice module to be dependend of alsa-source-old, which is obviously not good.
                  So the real fix should go to alsa-source-old.
                */
                if (latency > 10000)
                    latency -= 5000;
                latency += pa_bytes_to_usec((uint64_t)pa_memblockq_get_length(u->hw_source_memblockq),
                                            &u->hw_source_output->thread_info.sample_spec);
                pa_rtclock_get(&tv_ul_tstamp);
                pa_timeval_sub(&tv_ul_tstamp, latency + r->loop_padding_usec);
                pa_usec_t loop_padding_time = pa_timeval_diff(&tv_ul_tstamp, &tv_dl_tstamp);
                size_t loop_padding_bytes = pa_usec_to_bytes_round_up(loop_padding_time, &u->aep_sample_spec);

                pa_log_debug("Ear ref loop padding %d.%06d - %d.%06d = %lld = %d bytes (%lld latency %d extra padding)",
                     (int)tv_ul_tstamp.tv_sec, (int)tv_ul_tstamp.tv_usec,
                     (int)tv_dl_tstamp.tv_sec, (int)tv_dl_tstamp.tv_usec,
                     loop_padding_time, loop_padding_bytes,
                     latency, r->loop_padding_usec);
                if (0 > pa_timeval_cmp(&tv_dl_tstamp, &tv_ul_tstamp)) {
                pa_log_debug("Dl stamp precedes UL stamp %d.%06d < %d.%06d, something went wrong -> reset",
                         (int)tv_dl_tstamp.tv_sec, (int)tv_dl_tstamp.tv_usec,
                         (int)tv_ul_tstamp.tv_sec, (int)tv_ul_tstamp.tv_usec);
                pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
                break;
            }

            if (loop_padding_bytes >= pa_memblockq_get_maxlength(r->loop_memblockq)) {
                pa_log_debug("Too long loop time %lld, reset init sequence", loop_padding_time);
                pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
                break;
            }

            pa_memchunk schunk;
            pa_silence_memchunk_get(&u->core->silence_cache,
                    u->core->mempool,
                    &schunk,
                    &u->aep_sample_spec,
                    loop_padding_bytes);

            if (pa_memblockq_push(r->loop_memblockq, &schunk) < 0) {
            pa_log_debug("Failed to push %d bytes of ear ref padding to memblockq (len %d max %d )",
                     loop_padding_bytes,
                     pa_memblockq_get_length(r->loop_memblockq),
                     pa_memblockq_get_maxlength(r->loop_memblockq));
            }
            pa_memblock_unref(schunk.memblock);
            VOICE_TIMEVAL_INVALIDATE(&r->loop_tstamp);
            pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RUNNING);
            pa_log_debug("Ear ref loop init sequence ready.");
            }
            break;
            case VOICE_EAR_REF_QUIT:
            case VOICE_EAR_REF_UL_READY: {
                //pa_log_debug("Still waiting for loop initialization data from DL");
                *chunk = u->aep_silence_memchunk;
                pa_memblock_ref(chunk->memblock);
                ret = -1;
            }
            break;
        }
    }
    return ret;
}

static inline
void voice_aep_ear_ref_ul_drop(struct userdata *u, pa_usec_t drop_usecs) {
    struct voice_aep_ear_ref *r = &u->ear_ref;
    size_t drop_bytes = pa_usec_to_bytes_round_up(drop_usecs, &u->aep_sample_spec);
    if (pa_memblockq_get_length(r->loop_memblockq) >= drop_bytes + u->aep_fragment_size)
        pa_memblockq_drop(r->loop_memblockq, drop_bytes);
    else
        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
}

static inline
void voice_aep_ear_ref_ul_drop_log(struct userdata *u, pa_usec_t drop_usecs) {
    struct voice_aep_ear_ref *r = &u->ear_ref;
    size_t drop_bytes = pa_usec_to_bytes_round_up(drop_usecs, &u->aep_sample_spec);
    if (pa_memblockq_get_length(r->loop_memblockq) >= drop_bytes + u->aep_fragment_size) {
        pa_memblockq_drop(r->loop_memblockq, drop_bytes);
        pa_log_debug("Dropped %lld usec = %d bytes, %d bytes left in loop", drop_usecs, drop_bytes,
                     pa_memblockq_get_length(r->loop_memblockq));
    } else {
        pa_log_debug("Not enough bytes in ear ref loop %d < %d + %d, resetting",
                     pa_memblockq_get_length(r->loop_memblockq), drop_bytes, u->aep_fragment_size);
        pa_atomic_store(&r->loop_state, VOICE_EAR_REF_RESET);
    }
}


#endif // voice_aep_ear_ref_h
