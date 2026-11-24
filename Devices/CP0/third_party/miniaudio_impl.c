/*
 * miniaudio single-header implementation unit for the CP0 (CardputerZero/Linux) audio backend.
 *
 * The CP0 host port plays and records through the system PipeWire/PulseAudio graph (the same
 * route the factory Piano app uses: miniaudio -> libpulse0 -> pipewire-pulse -> ES8389), so the
 * app depends only on libpulse0 (present in the base image) instead of libasound2 + pipewire-alsa.
 *
 * Compiled once here; the rest of the tree only needs the declarations in miniaudio.h.
 */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

/* PulseAudio backend only: libpulse.so.0 is dlopen'd at runtime (Depends: libpulse0). */
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
