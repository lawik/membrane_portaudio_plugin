#pragma once

#include <membrane/membrane.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra" 
#include <portaudio.h>
#pragma GCC diagnostic pop
#include <stdio.h>
#include <string.h>

#include "pa_helper.h"

// Everything the PortAudio callback works on. It lives outside of the resource
// because the callback keeps running with a pointer to it after the resource
// has been garbage collected, until the SyncExecutor stops the stream in
// destroy.
typedef struct _SourceCtx {
  int channels;
  int frame_size;
  PaStream *stream;
  UnifexPid destination; // Where capture thread will send messages
} SourceCtx;

typedef struct _SourceState {
  int is_content_destroyed;
  SourceCtx *ctx;
} SourceState;

#include "_generated/source.h"
