#include "source.h"
#define MEMBRANE_LOG_TAG "Membrane.PortAudio.Sink"
#include <membrane/log.h>

void handle_destroy_state(UnifexEnv *env, SourceState *state) {
  if (state->is_content_destroyed) return;
  // The stream is still running and its callback uses state->ctx, so the
  // context has to stay alive until the SyncExecutor has stopped the stream.
  // Hand it over in a fresh state, this one is freed as soon as we return.
  SourceState *temp_state = unifex_alloc_state(env);
  memcpy(temp_state, state, sizeof(SourceState));

  UnifexPid exec_pid;
  if (!unifex_get_pid_by_name(env, "Elixir.Membrane.PortAudio.SyncExecutor", 0,
                              &exec_pid) ||
      !send_destroy(env, exec_pid, 0, temp_state)) {
    MEMBRANE_WARN(env, "Failed to destroy state");
    // Nobody is going to stop the stream, so the context must not be freed
    temp_state->is_content_destroyed = 1;
  }
  // The message holds its own reference to temp_state
  unifex_release_state(env, temp_state);
}

static int callback(const void *input_buffer, void *_output_buffer,
                    unsigned long frames,
                    const PaStreamCallbackTimeInfo *_time_info,
                    PaStreamCallbackFlags _flags, void *user_data) {
  UNIFEX_UNUSED(_output_buffer);
  UNIFEX_UNUSED(_time_info);
  UNIFEX_UNUSED(_flags);

  SourceCtx *ctx = (SourceCtx *)user_data;
  UnifexEnv *env = unifex_alloc_env(NULL);

  UnifexPayload payload;
  unifex_payload_alloc(env, UNIFEX_PAYLOAD_BINARY, frames * ctx->frame_size,
                       &payload);
  memcpy(payload.data, input_buffer, payload.size);
  if (!send_portaudio_payload(env, ctx->destination, UNIFEX_SEND_THREADED,
                              &payload)) {
    MEMBRANE_THREADED_WARN(env, "Payload send failed");
  }
  unifex_payload_release(&payload);

  unifex_free_env(env);

  return paContinue;
}

UNIFEX_TERM create(UnifexEnv *env, UnifexPid destination, int device_id,
                   int pa_buffer_size, char *latency, char *sample_format_str,
                   int channels, int sample_rate_int) {
  MEMBRANE_DEBUG(env, "Initializing");

  SourceState *state = unifex_alloc_state(env);
  state->is_content_destroyed = 0;
  state->ctx = unifex_alloc(sizeof(SourceCtx));
  if (!state->ctx) {
    MEMBRANE_WARN(env, "Error allocating stream context");
    state->is_content_destroyed = 1;
    unifex_release_state(env, state);
    return create_result_error(env, "alloc_error");
  }

  SourceCtx *ctx = state->ctx;
  ctx->destination = destination;
  ctx->stream = NULL;
  ctx->channels = 0;
  ctx->frame_size = 0;

  double sample_rate = (double)sample_rate_int;
  PaSampleFormat sample_format = string_to_PaSampleFormat(sample_format_str);

  int _latency_ms;
  char *error =
      init_pa(env, MEMBRANE_LOG_TAG, STREAM_DIRECTION_IN, &(ctx->stream), ctx,
              sample_format, &sample_rate, &channels, latency, &_latency_ms,
              pa_buffer_size, device_id, callback);

  ctx->channels = channels;
  ctx->frame_size = channels * sample_size(sample_format);
  UNIFEX_TERM res =
      error ? create_result_error(env, error)
            : create_result_ok(env, state, channels, floor(sample_rate));
  unifex_release_state(env, state);
  return res;
}

UNIFEX_TERM destroy(UnifexEnv *env, SourceState *state) {
  SourceCtx *ctx = state->ctx;
  if (ctx) {
    // Stops the stream first, so the callback no longer uses the context
    destroy_pa(env, MEMBRANE_LOG_TAG, ctx->stream);
    ctx->stream = NULL;
    unifex_free(ctx);
    state->ctx = NULL;
  }
  state->is_content_destroyed = 1;

  return destroy_result(env);
}
