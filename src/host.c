/*
 * Copyright (C) 2017 Ben Smith
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include "host.h"

#include <assert.h>

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

#include "emulator.h"

#define HOOK0(name)                           \
  do                                          \
    if (host->init.hooks.name) {              \
      host->init.hooks.name(&host->hook_ctx); \
    }                                         \
  while (0)

#define HOOK(name, ...)                                    \
  do                                                       \
    if (host->init.hooks.name) {                           \
      host->init.hooks.name(&host->hook_ctx, __VA_ARGS__); \
    }                                                      \
  while (0)

#define FOREACH_GLEXT_PROC(V)                                    \
  V(glAttachShader, PFNGLATTACHSHADERPROC)                       \
  V(glBindBuffer, PFNGLBINDBUFFERPROC)                           \
  V(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC)                 \
  V(glBufferData, PFNGLBUFFERDATAPROC)                           \
  V(glCompileShader, PFNGLCOMPILESHADERPROC)                     \
  V(glCreateProgram, PFNGLCREATEPROGRAMPROC)                     \
  V(glCreateShader, PFNGLCREATESHADERPROC)                       \
  V(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
  V(glGenBuffers, PFNGLGENBUFFERSPROC)                           \
  V(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC)                 \
  V(glGetAttribLocation, PFNGLGETATTRIBLOCATIONPROC)             \
  V(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC)             \
  V(glGetProgramiv, PFNGLGETPROGRAMIVPROC)                       \
  V(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC)               \
  V(glGetShaderiv, PFNGLGETSHADERIVPROC)                         \
  V(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC)           \
  V(glLinkProgram, PFNGLLINKPROGRAMPROC)                         \
  V(glShaderSource, PFNGLSHADERSOURCEPROC)                       \
  V(glUniform1i, PFNGLUNIFORM1IPROC)                             \
  V(glUniformMatrix3fv, PFNGLUNIFORMMATRIX3FVPROC)               \
  V(glUseProgram, PFNGLUSEPROGRAMPROC)                           \
  V(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC)

#define V(name, type) type name;
FOREACH_GLEXT_PROC(V)
#undef V

#define DESTROY_IF(ptr, destroy) \
  if (ptr) {                     \
    destroy(ptr);                \
    ptr = NULL;                  \
  }

#define AUDIO_SPEC_FORMAT AUDIO_U16
#define AUDIO_SPEC_CHANNELS 2
#define AUDIO_SPEC_SAMPLE_SIZE sizeof(HostAudioSample)
#define AUDIO_FRAME_SIZE (AUDIO_SPEC_SAMPLE_SIZE * AUDIO_SPEC_CHANNELS)
#define AUDIO_CONVERT_SAMPLE_FROM_U8(X) ((X) << 8)
typedef u16 HostAudioSample;
#define AUDIO_TARGET_QUEUED_SIZE (2 * host->audio.spec.size)
#define AUDIO_MAX_QUEUED_SIZE (5 * host->audio.spec.size)

#define TEXTURE_WIDTH 256
#define TEXTURE_HEIGHT 256

#define CHECK_LOG(var, kind, status_enum, kind_str)      \
  do {                                                   \
    GLint status;                                        \
    glGet##kind##iv(var, status_enum, &status);          \
    if (!status) {                                       \
      GLint length;                                      \
      glGet##kind##iv(var, GL_INFO_LOG_LENGTH, &length); \
      GLchar* log = malloc(length + 1); /* Leaks. */     \
      glGet##kind##InfoLog(var, length, NULL, log);      \
      PRINT_ERROR(kind_str " ERROR: %s\n", log);         \
      goto error;                                        \
    }                                                    \
  } while (0)

#define COMPILE_SHADER(var, type, source)           \
  GLuint var = glCreateShader(type);                \
  glShaderSource(var, 1, &(source), NULL);          \
  glCompileShader(var);                             \
  CHECK_LOG(var, Shader, GL_COMPILE_STATUS, #type); \
  glAttachShader(host->program, var);

typedef struct {
  SDL_AudioDeviceID dev;
  SDL_AudioSpec spec;
  u8* buffer; /* Size is spec.size. */
  Bool ready;
} HostAudio;

typedef struct {
  f32 pos[2];
  f32 tex_coord[2];
} HostVertex;

typedef struct Host {
  HostInit init;
  HostConfig config;
  HostHookContext hook_ctx;
  SDL_Window* window;
  SDL_GLContext gl_context;
  HostAudio audio;
  u64 start_counter;
  u64 performance_frequency;
  HostVertex vertices[4];
  f32 proj_matrix[9];
  GLuint vao;
  GLuint vbo;
  GLuint texture;
  GLuint program;
  GLint uProjMatrix;
  GLint uSampler;
} Host;

Result host_init_video(Host* host) {
  static const char* s_vertex_shader =
      "attribute vec2 aPos;\n"
      "attribute vec2 aTexCoord;\n"
      "varying vec2 vTexCoord;\n"
      "uniform mat3 uProjMatrix;\n"
      "void main(void) {\n"
      "  gl_Position = vec4(uProjMatrix * vec3(aPos, 1.0), 1.0);\n"
      "  vTexCoord = aTexCoord;\n"
      "}\n";

  static const char* s_fragment_shader =
      "varying vec2 vTexCoord;\n"
      "uniform sampler2D uSampler;\n"
      "void main(void) {\n"
      "  gl_FragColor = texture2D(uSampler, vTexCoord);\n"
      "}\n";

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  host->window = SDL_CreateWindow("binjgb", SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SCREEN_WIDTH * host->init.render_scale,
                                  SCREEN_HEIGHT * host->init.render_scale,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  CHECK_MSG(host->window != NULL, "SDL_CreateWindow failed.\n");

  host->gl_context = SDL_GL_CreateContext(host->window);
  GLint major;
  SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
  CHECK_MSG(major >= 2, "Unable to create GL context at version 2.\n");
#define V(name, type)                  \
  name = SDL_GL_GetProcAddress(#name); \
  CHECK_MSG(name != 0, "Unable to get GL function: " #name);
  FOREACH_GLEXT_PROC(V)
#undef V

  glGenBuffers(1, &host->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, host->vbo);

  glGenTextures(1, &host->texture);
  glBindTexture(GL_TEXTURE_2D, host->texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXTURE_WIDTH, TEXTURE_HEIGHT, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  host->program = glCreateProgram();
  COMPILE_SHADER(vertex_shader, GL_VERTEX_SHADER, s_vertex_shader);
  COMPILE_SHADER(fragment_shader, GL_FRAGMENT_SHADER, s_fragment_shader);
  glLinkProgram(host->program);
  CHECK_LOG(host->program, Program, GL_LINK_STATUS, "GL_PROGRAM");

  GLint aPos = glGetAttribLocation(host->program, "aPos");
  GLint aTexCoord = glGetAttribLocation(host->program, "aTexCoord");
  host->uProjMatrix = glGetUniformLocation(host->program, "uProjMatrix");
  host->uSampler = glGetUniformLocation(host->program, "uSampler");

  glGenVertexArrays(1, &host->vao);
  glBindVertexArray(host->vao);
  glEnableVertexAttribArray(aPos);
  glEnableVertexAttribArray(aTexCoord);
  glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, sizeof(HostVertex),
                        (void*)offsetof(HostVertex, pos));
  glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, sizeof(HostVertex),
                        (void*)offsetof(HostVertex, tex_coord));

  return OK;
error:
  SDL_Quit();
  return ERROR;
}

static void host_init_time(Host* host) {
  host->performance_frequency = SDL_GetPerformanceFrequency();
  host->start_counter = SDL_GetPerformanceCounter();
}

f64 host_get_time_ms(Host* host) {
  u64 now = SDL_GetPerformanceCounter();
  return (f64)(now - host->start_counter) * 1000 / host->performance_frequency;
}

static Result host_init_audio(Host* host) {
  host->audio.ready = FALSE;
  SDL_AudioSpec want;
  want.freq = host->init.audio_frequency;
  want.format = AUDIO_SPEC_FORMAT;
  want.channels = AUDIO_SPEC_CHANNELS;
  want.samples = host->init.audio_frames * AUDIO_SPEC_CHANNELS;
  want.callback = NULL;
  want.userdata = host;
  host->audio.dev = SDL_OpenAudioDevice(NULL, 0, &want, &host->audio.spec, 0);
  CHECK_MSG(host->audio.dev != 0, "SDL_OpenAudioDevice failed.\n");

  host->audio.buffer = calloc(1, host->audio.spec.size);
  CHECK_MSG(host->audio.buffer != NULL, "Audio buffer allocation failed.\n");
  return OK;
  ON_ERROR_RETURN;
}

Bool host_poll_events(Host* host) {
  struct Emulator* e = host->hook_ctx.e;
  Bool running = TRUE;
  SDL_Event event;
  EmulatorConfig emu_config = emulator_get_config(e);
  HostConfig host_config = host_get_config(host);
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_WINDOWEVENT:
        switch (event.window.event) {
          case SDL_WINDOWEVENT_RESIZED: {
            f32 w = event.window.data1;
            f32 h = event.window.data2;
            glViewport(0, 0, w, h);

            memset(host->proj_matrix, 0, sizeof(host->proj_matrix));
            host->proj_matrix[0] = 2.0f / w;
            host->proj_matrix[4] = -2.0f / h;
            host->proj_matrix[6] = -1.0f;
            host->proj_matrix[7] = 1.0f;
            host->proj_matrix[8] = 1.0f;

            f32 aspect = w / h, want_aspect = (f32)SCREEN_WIDTH / SCREEN_HEIGHT;
            f32 new_w = aspect < want_aspect ? w : h * want_aspect;
            f32 new_h = aspect < want_aspect ? w / want_aspect : h;
            f32 new_left = (w - new_w) * 0.5f;
            f32 new_right = new_left + new_w;
            f32 new_top = (h - new_h) * 0.5f;
            f32 new_bottom = new_top + new_h;
            f32 u_right = (f32)SCREEN_WIDTH / TEXTURE_WIDTH;
            f32 v_bottom = (f32)SCREEN_HEIGHT / TEXTURE_HEIGHT;

#define SET_VERTEX(index, x_val, y_val, u_val, v_val) \
  host->vertices[index].pos[0] = x_val;               \
  host->vertices[index].pos[1] = y_val;               \
  host->vertices[index].tex_coord[0] = u_val;         \
  host->vertices[index].tex_coord[1] = v_val

            SET_VERTEX(0, new_left, new_top, 0, 0);
            SET_VERTEX(1, new_left, new_bottom, 0, v_bottom);
            SET_VERTEX(2, new_right, new_top, u_right, 0);
            SET_VERTEX(3, new_right, new_bottom, u_right, v_bottom);

#undef SET_VERTEX

            break;
          }
        }
        break;
      case SDL_KEYDOWN:
        switch (event.key.keysym.scancode) {
          case SDL_SCANCODE_1: emu_config.disable_sound[CHANNEL1] ^= 1; break;
          case SDL_SCANCODE_2: emu_config.disable_sound[CHANNEL2] ^= 1; break;
          case SDL_SCANCODE_3: emu_config.disable_sound[CHANNEL3] ^= 1; break;
          case SDL_SCANCODE_4: emu_config.disable_sound[CHANNEL4] ^= 1; break;
          case SDL_SCANCODE_B: emu_config.disable_bg ^= 1; break;
          case SDL_SCANCODE_W: emu_config.disable_window ^= 1; break;
          case SDL_SCANCODE_O: emu_config.disable_obj ^= 1; break;
          case SDL_SCANCODE_F6: HOOK0(write_state); break;
          case SDL_SCANCODE_F9: HOOK0(read_state); break;
          case SDL_SCANCODE_N:
            host_config.step = 1;
            host_config.paused = 0;
            break;
          case SDL_SCANCODE_SPACE: host_config.paused ^= 1; break;
          case SDL_SCANCODE_ESCAPE: running = FALSE; break;
          default: break;
        }
        /* fall through */
      case SDL_KEYUP: {
        Bool down = event.type == SDL_KEYDOWN;
        switch (event.key.keysym.scancode) {
          case SDL_SCANCODE_TAB: host_config.no_sync = down; break;
          case SDL_SCANCODE_F11: if (!down) host_config.fullscreen ^= 1; break;
          default: break;
        }
        break;
      }
      case SDL_QUIT: running = FALSE; break;
      default: break;
    }
  }
  emulator_set_config(e, &emu_config);
  host_set_config(host, &host_config);
  return running;
}

void host_upload_video(Host* host) {
  struct Emulator* e = host->hook_ctx.e;
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_RGBA,
                  GL_UNSIGNED_BYTE, emulator_get_frame_buffer(e));
}

void host_render_video(Host* host) {
  glClearColor(0.1f, 0.1f, 0.1f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(host->program);
  glUniformMatrix3fv(host->uProjMatrix, 1, GL_FALSE, host->proj_matrix);
  glUniform1i(host->uSampler, 0);
  glBindVertexArray(host->vao);
  glBindBuffer(GL_ARRAY_BUFFER, host->vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(host->vertices), host->vertices,
               GL_DYNAMIC_DRAW);
  glBindTexture(GL_TEXTURE_2D, host->texture);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  SDL_GL_SwapWindow(host->window);
}

static void host_reset_audio(Host* host) {
  host->audio.ready = FALSE;
  SDL_ClearQueuedAudio(host->audio.dev);
  SDL_PauseAudioDevice(host->audio.dev, 1);
}

void host_render_audio(Host* host) {
  struct Emulator* e = host->hook_ctx.e;
  HostAudio* audio = &host->audio;
  AudioBuffer* audio_buffer = emulator_get_audio_buffer(e);

  size_t src_frames = audio_buffer_get_frames(audio_buffer);
  size_t max_dst_frames = audio->spec.size / AUDIO_FRAME_SIZE;
  size_t frames = MIN(src_frames, max_dst_frames);
  u8* src = audio_buffer->data;
  HostAudioSample* dst = (HostAudioSample*)audio->buffer;
  HostAudioSample* dst_end = dst + frames * AUDIO_SPEC_CHANNELS;
  assert((u8*)dst_end <= audio->buffer + audio->spec.size);
  for (size_t i = 0; i < frames; i++) {
    assert(dst + 2 <= dst_end);
    *dst++ = AUDIO_CONVERT_SAMPLE_FROM_U8(*src++);
    *dst++ = AUDIO_CONVERT_SAMPLE_FROM_U8(*src++);
  }
  u32 queued_size = SDL_GetQueuedAudioSize(audio->dev);
  if (queued_size < AUDIO_MAX_QUEUED_SIZE) {
    u32 buffer_size = (u8*)dst_end - (u8*)audio->buffer;
    SDL_QueueAudio(audio->dev, audio->buffer, buffer_size);
    HOOK(audio_add_buffer, queued_size, queued_size + buffer_size);
    queued_size += buffer_size;
  }
  if (!audio->ready && queued_size >= AUDIO_TARGET_QUEUED_SIZE) {
    HOOK(audio_buffer_ready, queued_size);
    audio->ready = TRUE;
    SDL_PauseAudioDevice(audio->dev, 0);
  }
}

static void joypad_callback(JoypadButtons* joyp, void* user_data) {
  Host* sdl = user_data;
  const u8* state = SDL_GetKeyboardState(NULL);
  joyp->up = state[SDL_SCANCODE_UP];
  joyp->down = state[SDL_SCANCODE_DOWN];
  joyp->left = state[SDL_SCANCODE_LEFT];
  joyp->right = state[SDL_SCANCODE_RIGHT];
  joyp->B = state[SDL_SCANCODE_Z];
  joyp->A = state[SDL_SCANCODE_X];
  joyp->start = state[SDL_SCANCODE_RETURN];
  joyp->select = state[SDL_SCANCODE_BACKSPACE];
}

Result host_init(Host* host, struct Emulator* e) {
  CHECK_MSG(SDL_Init(SDL_INIT_EVERYTHING) == 0, "SDL_init failed.\n");
  host_init_time(host);
  CHECK(SUCCESS(host_init_video(host)));
  CHECK(SUCCESS(host_init_audio(host)));
  emulator_set_joypad_callback(e, joypad_callback, host);
  return OK;
  ON_ERROR_RETURN;
}

void host_run_ms(struct Host* host, f64 delta_ms) {
  if (host->config.paused) {
    return;
  }

  struct Emulator* e = host->hook_ctx.e;
  u32 delta_cycles = (u32)(delta_ms * CPU_CYCLES_PER_SECOND / 1000);
  u32 until_cycles = emulator_get_cycles(e) + delta_cycles;
  while (1) {
    EmulatorEvent event = emulator_run_until(e, until_cycles);
    if (event & EMULATOR_EVENT_NEW_FRAME) {
      host_upload_video(host);
    }
    if (event & EMULATOR_EVENT_AUDIO_BUFFER_FULL) {
      host_render_audio(host);
    }
    if (event & EMULATOR_EVENT_UNTIL_CYCLES) {
      break;
    }
  }
  HostConfig config = host_get_config(host);
  if (config.step) {
    config.paused = TRUE;
    config.step = FALSE;
    host_set_config(host, &config);
  }
}

Host* host_new(const HostInit *init, struct Emulator* e) {
  Host* host = calloc(1, sizeof(Host));
  host->init = *init;
  host->hook_ctx.host = host;
  host->hook_ctx.e = e;
  host->hook_ctx.user_data = host->init.hooks.user_data;
  CHECK(SUCCESS(host_init(host, e)));
  return host;
error:
  free(host);
  return NULL;
}

void host_delete(Host* host) {
  SDL_GL_DeleteContext(host->gl_context);
  SDL_DestroyWindow(host->window);
  SDL_Quit();
  free(host->audio.buffer);
  free(host);
}

void host_set_config(struct Host* host, const HostConfig* new_config) {
  if (host->config.no_sync != new_config->no_sync) {
    SDL_GL_SetSwapInterval(new_config->no_sync ? 0 : 1);
    host_reset_audio(host);
  }

  if (host->config.paused != new_config->paused) {
    host_reset_audio(host);
  }

  if (host->config.fullscreen != new_config->fullscreen) {
    SDL_SetWindowFullscreen(host->window, new_config->fullscreen
                                              ? SDL_WINDOW_FULLSCREEN_DESKTOP
                                              : 0);
  }
  host->config = *new_config;
}

HostConfig host_get_config(struct Host* host) {
  return host->config;
}

f64 host_get_monitor_refresh_ms(struct Host* host) {
  int refresh_rate_hz = 0;
  SDL_DisplayMode mode;
  if (SDL_GetWindowDisplayMode(host->window, &mode) == 0) {
    refresh_rate_hz = mode.refresh_rate;
  }
  if (refresh_rate_hz == 0) {
    refresh_rate_hz = 60;
  }
  return 1000.0 / refresh_rate_hz;
}
