#include "sink.h"
#define MEMBRANE_LOG_TAG "Membrane.PortAudio.Sink"
#include <membrane/log.h>

#define BUFFERS_PER_TICK 100

void handle_destroy_state(UnifexEnv *env, SinkState *state) {
  if (state->is_content_destroyed) return;
  // The stream is still running and its callback uses state->ctx, so the
  // context has to stay alive until the SyncExecutor has stopped the stream.
  // Hand it over in a fresh state, this one is freed as soon as we return.
  SinkState *temp_state = unifex_alloc_state(env);
  memcpy(temp_state, state, sizeof(SinkState));

  UnifexPid exec_pid;
  if (!unifex_get_pid_by_name(env, "Elixir.Membrane.PortAudio.SyncExecutor", 0,
                              &exec_pid) ||
      !send_destroy(env, exec_pid, 0, temp_state)) {
    MEMBRANE_WARN(env, "PortAudio sink: failed to destroy state");
    // Nobody is going to stop the stream, so the context must not be freed
    temp_state->is_content_destroyed = 1;
  }
  // The message holds its own reference to temp_state
  unifex_release_state(env, temp_state);
}

static int callback(const void *_input_buffer, void *output_buffer,
                    unsigned long frames_per_buffer,
                    const PaStreamCallbackTimeInfo *_time_info,
                    PaStreamCallbackFlags _flags, void *user_data) {
  UNIFEX_UNUSED(_input_buffer);
  UNIFEX_UNUSED(_time_info);
  UNIFEX_UNUSED(_flags);

  UnifexEnv *env = unifex_alloc_env(NULL);
  SinkCtx *ctx = (SinkCtx *)user_data;

  if (++ctx->ticks % BUFFERS_PER_TICK == 0) {
    send_membrane_clock_update(env, ctx->membrane_clock, UNIFEX_SEND_THREADED,
                               BUFFERS_PER_TICK * frames_per_buffer,
                               ctx->sample_rate / 1000);
  }

  size_t elements_available =
      membrane_ringbuffer_get_read_available(ctx->ringbuffer);
  if (elements_available >= frames_per_buffer) {
    size_t elements_read = membrane_ringbuffer_read(
        ctx->ringbuffer, output_buffer, frames_per_buffer);
    if (ctx->demand + elements_read > ctx->ringbuffer->max_elements / 2) {
      if (!send_portaudio_demand(
              env, ctx->demand_handler, UNIFEX_SEND_THREADED,
              (ctx->demand + elements_read) * ctx->frame_size)) {
        MEMBRANE_THREADED_WARN(env, "PortAudio sink: failed to send demand");
      }
      ctx->demand = 0;
    } else {
      ctx->demand += elements_read;
    }
  } else {
    memset(output_buffer, 0, frames_per_buffer * ctx->frame_size);
  }
  unifex_free_env(env);
  return paContinue;
}

UNIFEX_TERM create(UnifexEnv *env, UnifexPid demand_handler,
                   UnifexPid membrane_clock, int device_id, int sample_rate,
                   int channels, char *sample_format_str, int ringbuffer_size,
                   int pa_buffer_size, char *latency) {
  MEMBRANE_DEBUG(env, "initializing");

  char *error;
  SinkState *state = NULL;
  int latency_ms;
  UNIFEX_TERM res;

  PaSampleFormat sample_format = string_to_PaSampleFormat(sample_format_str);

  if (sample_format == UNSUPPORTED_SAMPLE_FORMAT) {
    error = "Unsupported sample format";
    goto error;
  }

  int frame_size = channels * sample_size(sample_format);

  MembraneRingBuffer *ringbuffer =
      membrane_ringbuffer_new(ringbuffer_size, frame_size);
  if (!ringbuffer) {
    MEMBRANE_WARN(env, "Error initializing ringbuffer");
    error = "ringbuffer_init";
    goto error;
  }

  send_portaudio_demand(env, demand_handler, UNIFEX_NO_FLAGS,
                        ringbuffer_size * frame_size);

  state = unifex_alloc_state(env);
  state->is_content_destroyed = 0;
  state->ctx = unifex_alloc(sizeof(SinkCtx));
  if (!state->ctx) {
    MEMBRANE_WARN(env, "Error allocating stream context");
    membrane_ringbuffer_destroy(ringbuffer);
    state->is_content_destroyed = 1;
    error = "alloc_error";
    goto error;
  }

  SinkCtx *ctx = state->ctx;
  ctx->ringbuffer = ringbuffer;
  ctx->demand_handler = demand_handler;
  ctx->membrane_clock = membrane_clock;
  ctx->stream = NULL;
  ctx->demand = 0;
  ctx->ticks = 0;
  ctx->frame_size = frame_size;
  ctx->sample_rate = sample_rate;

  double sample_rate_double = sample_rate * 1.0;

  error = init_pa(env, MEMBRANE_LOG_TAG, STREAM_DIRECTION_OUT, &(ctx->stream),
                  ctx, sample_format, &sample_rate_double, &channels, latency,
                  &latency_ms, pa_buffer_size, device_id, callback);

  if (error) {
    goto error;
  }

error:

  res = error ? create_result_error(env, error)
              : create_result_ok(env, latency_ms, state);

  if (state) {
    unifex_release_state(env, state);
  }

  return res;
}

UNIFEX_TERM write_data(UnifexEnv *env, UnifexPayload *payload,
                       SinkState *state) {
  SinkCtx *ctx = state->ctx;
  size_t elements_written = membrane_ringbuffer_write(
      ctx->ringbuffer, payload->data, payload->size / ctx->frame_size);
  if (elements_written != payload->size / ctx->frame_size) {
    MEMBRANE_WARN(env,
                  "Write: written only %d out of %lu bytes into ringbuffer",
                  elements_written * ctx->frame_size, payload->size);
    return write_data_result_error_overrun(env);
  }
  return write_data_result_ok(env);
}

UNIFEX_TERM destroy(UnifexEnv *env, SinkState *state) {
  SinkCtx *ctx = state->ctx;
  if (ctx) {
    // Stops the stream first, so the callback no longer uses the context
    destroy_pa(env, MEMBRANE_LOG_TAG, ctx->stream);
    ctx->stream = NULL;

    if (ctx->ringbuffer) {
      membrane_ringbuffer_destroy(ctx->ringbuffer);
      ctx->ringbuffer = NULL;
    }

    unifex_free(ctx);
    state->ctx = NULL;
  }

  state->is_content_destroyed = 1;

  return destroy_result(env);
}
