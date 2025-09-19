#include "melonds-highscore.h"

#include "FreeBIOS.h"
#include "GPU.h"
#include "GPU3D_Compute.h"
#include "GPU3D_OpenGL.h"
#include "GPU3D_Soft.h"
#include "NDS.h"
#include "Platform.h"
#include "SPI.h"
#include "SPU.h"

#include <cmath>

#include "glad/glad.h"

#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 192
#define SAMPLE_RATE 48000
#define MAX_SAMPLES 2500
#define VOLUME_MULTIPLIER 1.5
#define N_BAD_FRAMES 1

#define USE_GL 0
#define USE_COMPUTE 0

using namespace melonDS;

struct _melonDSCore
{
  HsCore parent_instance;

  NDS *console;
  char *rom_path;
  char *save_path;

  HsGLContext *gl_context;
  gboolean compute;
  int skip_frames;

  GLuint vertex_buffer;
  GLuint vertex_array;
  GLuint program;

  HsSoftwareContext *context;

  gint16 *audio_buffer;
  gboolean mic_active;
};

static HsCore *core;

static void melonds_nintendo_ds_core_init (HsNintendoDsCoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (melonDSCore, melonds_core, HS_TYPE_CORE,
                               G_IMPLEMENT_INTERFACE (HS_TYPE_NINTENDO_DS_CORE, melonds_nintendo_ds_core_init))

static const char *VERTEX_SHADER = R"(#version 140

in vec2 vPosition;
in vec2 vTexcoord;

smooth out vec2 fTexcoord;

void main()
{
  gl_Position = vec4(vPosition * 2.0 - 1.0, 0.0, 1.0);
  fTexcoord = vTexcoord;
}
)";

static const char *FRAGMENT_SHADER = R"(#version 140

uniform sampler2D ScreenTex;

smooth in vec2 fTexcoord;

out vec4 oColor;

void main()
{
  vec4 pixel = texture(ScreenTex, fTexcoord);

  oColor = vec4(pixel.bgr, 1.0);
}
)";

static void
gl_init (melonDSCore *self)
{
  OpenGL::CompileVertexFragmentProgram (self->program,
                                        VERTEX_SHADER, FRAGMENT_SHADER,
                                        "ScreenShader",
                                        {{"vPosition", 0}, {"vTexcoord", 1}},
                                        {{"oColor", 0}});

  glUseProgram (self->program);
  glUniform1i (glGetUniformLocation (self->program, "ScreenTex"), 0);

  const int padded_height = SCREEN_HEIGHT * 2 + 2;
  const float pad_pixels = 1.f / padded_height;

  const float vertices[] = {
    0.f, 0.f,  0.f, 0.f,
    0.f, 0.5f, 0.f, 0.5f - pad_pixels,
    1.f, 0.5f, 1.f, 0.5f - pad_pixels,
    0.f, 0.f,  0.f, 0.f,
    1.f, 0.5f, 1.f, 0.5f - pad_pixels,
    1.f, 0.f,  1.f, 0.f,

    0.f, 0.5f, 0.f, 0.5f + pad_pixels,
    0.f, 1.f,  0.f, 1.f,
    1.f, 1.f,  1.f, 1.f,
    0.f, 0.5f, 0.f, 0.5f + pad_pixels,
    1.f, 1.f,  1.f, 1.f,
    1.f, 0.5f, 1.f, 0.5f + pad_pixels,
  };

  glGenBuffers (1, &self->vertex_buffer);
  glBindBuffer (GL_ARRAY_BUFFER, self->vertex_buffer);
  glBufferData (GL_ARRAY_BUFFER, sizeof (vertices), vertices, GL_STATIC_DRAW);

  glGenVertexArrays (1, &self->vertex_array);
  glBindVertexArray (self->vertex_array);
  glEnableVertexAttribArray (0); // position
  glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(0));
  glEnableVertexAttribArray (1); // texcoord
  glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(2*4));
}

static void
gl_draw_frame (melonDSCore *self)
{
  int front_buf = self->console->GPU.FrontBuffer;
  if (!self->console->GPU.Framebuffer[front_buf][0].get () || !self->console->GPU.Framebuffer[front_buf][1].get ())
    return;

  glBindFramebuffer (GL_FRAMEBUFFER, hs_gl_context_get_default_framebuffer (self->gl_context));
  glDisable (GL_DEPTH_TEST);
  glDepthMask (false);
  glDisable (GL_BLEND);
  glDisable (GL_SCISSOR_TEST);
  glDisable (GL_STENCIL_TEST);
  glClearColor (0, 0, 0, 1);
  glClear (GL_COLOR_BUFFER_BIT);

  glViewport (0, 0, SCREEN_WIDTH, SCREEN_HEIGHT * 2);
  glUseProgram (self->program);
  glActiveTexture (GL_TEXTURE0);

  self->console->GPU.GetRenderer3D ().BindOutputTexture (front_buf);

  glBindBuffer (GL_ARRAY_BUFFER, self->vertex_buffer);
  glBindVertexArray (self->vertex_array);

  glDrawArrays (GL_TRIANGLES, 0, 12);

  glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
  glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

static gpointer
get_proc_address (const char *name)
{
  melonDSCore *self = MELONDS_CORE (core);

  return hs_gl_context_get_proc_address (self->gl_context, name);
}

static char *
get_rom_basename (const char *rom_path)
{
  g_autoptr (GFile) rom = g_file_new_for_path (rom_path);
  char *basename = g_file_get_basename (rom);
  char *extension = strrchr (basename, '.');

  *extension = '\0';

  return basename;
}

static gboolean
try_migrate_desmume_save (const char *rom_path, const char *save_path, GError **error)
{
  g_autoptr (GFile) save_dir = g_file_new_for_path (save_path);
  if (!g_file_query_exists (save_dir, NULL)) {
    // No save dir, exiting
    return TRUE;
  }

  g_autoptr (GFile) dst_file = g_file_get_child (save_dir, "save.sav");
  if (g_file_query_exists (dst_file, NULL)) {
    // A raw save file already exists, exiting
    return TRUE;
  }

  g_autoptr (GFile) save_dsv = g_file_get_child (save_dir, "save.dsv");

  g_autofree char *basename = get_rom_basename (rom_path);
  g_autofree char *basename_dsv_name = g_strconcat (basename, ".dsv", NULL);
  g_autoptr (GFile) basename_dsv = g_file_get_child (save_dir, basename_dsv_name);

  GFile *src_file;

  if (g_file_query_exists (save_dsv, NULL)) {
    src_file = save_dsv;
  } else if (g_file_query_exists (basename_dsv, NULL)) {
    src_file = basename_dsv;
  } else {
    // No desmume save files, exiting
    return TRUE;
  }

  // Found both the source and destination file, migrating

  g_autofree char *contents = NULL;
  gsize contents_length;
  if (!g_file_load_contents (src_file, NULL, &contents, &contents_length, NULL, error))
    return FALSE;

  // Let's do a few additional checks

  if (contents_length < 0x7A) {
    // Too short to be a desmume save file
    return TRUE;
  }

  if (strncmp (&contents[contents_length - 0x10], "|-DESMUME SAVE-|", 0x10) != 0) {
    // Doesn't have desmume header (footer?)
    return TRUE;
  }

  // Writing a raw file
  if (!g_file_replace_contents (dst_file, contents, contents_length - 0x7A, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error))
    return FALSE;

  // Delete the desmume file
  if (!g_file_delete (src_file, NULL, error))
    return FALSE;

  // Success
  hs_core_log (HS_CORE (core), HS_LOG_MESSAGE, "Migrated '%s' to '%s'", g_file_peek_path (src_file), g_file_peek_path (dst_file));

  return TRUE;
}

static gboolean
melonds_core_load_rom (HsCore      *core,
                       const char **rom_paths,
                       int          n_rom_paths,
                       const char  *save_path,
                       GError     **error)
{
  melonDSCore *self = MELONDS_CORE (core);

  g_assert (n_rom_paths == 1);

  g_set_str (&self->rom_path, rom_paths[0]);

  if (!try_migrate_desmume_save (self->rom_path, save_path, error))
    return FALSE;

  g_autoptr (GFile) save_dir = g_file_new_for_path (save_path);
  if (!g_file_query_exists (save_dir, NULL) && !g_file_make_directory_with_parents (save_dir, NULL, error))
    return FALSE;

  g_autoptr (GFile) save_file = g_file_get_child (save_dir, "save.sav");

  g_set_str (&self->save_path, g_file_get_path (save_file));

  NDSArgs nds_args = {};
  self->console = new NDS (std::move (nds_args), self);
  NDS::Current = self->console;

  if (!self->console) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to init the console");
    return FALSE;
  }

  const char *renderer_env = g_getenv ("HIGHSCORE_MELONDS_RENDERER");
  self->compute = !g_strcmp0 (renderer_env, "compute");
  gboolean use_gl = !g_strcmp0 (renderer_env, "gl") || self->compute;

  if (use_gl) {
    self->gl_context = hs_core_create_gl_context (core, HS_GL_API_GL, 3, 2, HS_GL_FLAGS_DEFAULT);

    if (hs_gl_context_realize (self->gl_context, NULL) && gladLoadGLLoader (get_proc_address)) {
      hs_gl_context_set_size (self->gl_context, SCREEN_WIDTH, SCREEN_HEIGHT * 2);

      if (self->compute) {
        auto renderer = ComputeRenderer::New ();
        renderer->SetRenderSettings (1, true);

        self->console->GPU.SetRenderer3D (std::move (renderer));

        hs_core_log_literal (core, HS_LOG_MESSAGE, "Using compute GL renderer");
      } else {
        auto renderer = GLRenderer::New ();
        renderer->SetRenderSettings (false, 1);

        self->console->GPU.SetRenderer3D (std::move (renderer));

        hs_core_log_literal (core, HS_LOG_MESSAGE, "Using GL renderer");
      }

      gl_init (self);
    } else {
      hs_gl_context_unrealize (self->gl_context);
      g_clear_object (&self->gl_context);

      hs_core_log (core, HS_LOG_WARNING, "Failed to initialize GL context, falling back to software renderer");
    }
  }

  if (!self->gl_context) {
    self->context = hs_core_create_software_context (core, SCREEN_WIDTH, SCREEN_HEIGHT * 2, HS_PIXEL_FORMAT_B8G8R8X8);

    auto renderer = std::make_unique<SoftRenderer> ();
    self->console->GPU.SetRenderer3D (std::move (renderer));
  }

  g_autofree char *rom_data = NULL;
  gsize rom_length;
  if (!g_file_get_contents (self->rom_path, &rom_data, &rom_length, error))
    return FALSE;

  auto cart = NDSCart::ParseROM ((const u8*) rom_data, rom_length, self, std::nullopt);
  if (!cart) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to parse ROM");
    return FALSE;
  }

  if (g_file_query_exists (save_file, NULL)) {
    g_autofree char *save_data = NULL;
    gsize save_length = 0;

    if (!g_file_load_contents (save_file, NULL, &save_data, &save_length, NULL, error))
      return FALSE;

    cart->SetSaveMemory ((const u8*) save_data, save_length);
  }

  self->console->SetNDSCart (std::move (cart));
  self->console->Reset ();

  if (self->console->NeedsDirectBoot ())
    self->console->SetupDirectBoot ("");

  if (self->gl_context && self->compute)
    OpenGL::LoadShaderCache ();

  self->audio_buffer = g_new0 (gint16, MAX_SAMPLES);

  self->console->SPU.SetOutputSampleRate (SAMPLE_RATE);

  return TRUE;
}

static void
melonds_core_start (HsCore *core)
{
  melonDSCore *self = MELONDS_CORE (core);

  self->console->Start ();

  int current_shader, shaders_count;

  do {
    self->console->GPU.GetRenderer3D ().ShaderCompileStep (current_shader, shaders_count);
  } while (self->console->GPU.GetRenderer3D ().NeedsShaderCompile ());

  /* The first couple frames will be bad, skip them */
  self->skip_frames = N_BAD_FRAMES;
}

static gboolean
melonds_core_reset (HsCore *core, gboolean hard, GError **error)
{
  melonDSCore *self = MELONDS_CORE (core);

  self->console->Reset ();

  if (self->console->NeedsDirectBoot ())
    self->console->SetupDirectBoot ("");

  /* The first couple frames will be bad, skip them */
  self->skip_frames = N_BAD_FRAMES;
  return TRUE;
}

static void
melonds_core_stop (HsCore *core)
{
  melonDSCore *self = MELONDS_CORE (core);

  if (self->gl_context && self->compute)
    OpenGL::SaveShaderCache ();

  self->console->Halt ();
  self->console->Stop ();

  delete self->console;
  self->console = NULL;

  NDS::Current = NULL;

  if (self->gl_context) {
    glDeleteVertexArrays (1, &self->vertex_array);
    glDeleteBuffers (1, &self->vertex_buffer);
    glDeleteProgram (self->program);

    hs_gl_context_unrealize (self->gl_context);
    g_clear_object (&self->gl_context);
  }

  g_clear_object (&self->context);
  g_clear_pointer (&self->rom_path, g_free);
  g_clear_pointer (&self->save_path, g_free);
  g_clear_pointer (&self->audio_buffer, g_free);
}

const int BUTTON_MAPPING[] = {
  6, 7, 5,  4,  // UP, DOWN, LEFT, RIGHT
  0, 1, 10, 11, // A, B, X, Y
  2, 3,         // SELECT, START
  9, 8,         // L, R
};

static void
melonds_core_poll_input (HsCore *core, HsInputState *input_state)
{
  melonDSCore *self = MELONDS_CORE (core);
  u32 mask = 0xfff;

  for (int btn = 0; btn < HS_NINTENDO_DS_N_BUTTONS; btn++) {
    if (input_state->nintendo_ds.buttons & 1 << btn)
      mask &= ~(1 << BUTTON_MAPPING[btn]);
  }

  self->console->SetKeyMask (mask);

  if (input_state->nintendo_ds.touch_pressed) {
    u16 x = (u16) round (input_state->nintendo_ds.touch_x * SCREEN_WIDTH);
    u16 y = (u16) round (input_state->nintendo_ds.touch_y * SCREEN_HEIGHT);
    self->console->TouchScreen (x, y);
  } else {
    self->console->ReleaseScreen ();
  }

  self->mic_active = input_state->nintendo_ds.mic_active;
}

static void
melonds_core_run_frame (HsCore *core)
{
  melonDSCore *self = MELONDS_CORE (core);

  self->console->RunFrame ();

  u32 n_samples = self->console->SPU.GetOutputSize ();
  self->console->SPU.ReadOutput (self->audio_buffer, n_samples);

  for (int i = 0; i < n_samples * 2; i++)
    self->audio_buffer[i] *= VOLUME_MULTIPLIER;

  hs_core_play_samples (core, self->audio_buffer, n_samples * 2);

  if (self->skip_frames > 0) {
    self->skip_frames--;
    return;
  }

  if (self->gl_context) {
    gl_draw_frame (self);
    hs_gl_context_swap_buffers (self->gl_context);
    return;
  }

  size_t screen_size = (SCREEN_WIDTH * SCREEN_HEIGHT * 4);
  u8 *vbuf0 = (u8*) hs_software_context_acquire_framebuffer (self->context);
  u8 *vbuf1 = vbuf0 + screen_size;
  memcpy (vbuf0, self->console->GPU.Framebuffer[self->console->GPU.FrontBuffer][0].get (), screen_size);
  memcpy (vbuf1, self->console->GPU.Framebuffer[self->console->GPU.FrontBuffer][1].get (), screen_size);
  hs_software_context_release_framebuffer (self->context);
}

static gboolean
melonds_core_reload_save (HsCore      *core,
                          const char  *save_path,
                          GError     **error)
{
  melonDSCore *self = MELONDS_CORE (core);

  if (!try_migrate_desmume_save (self->rom_path, save_path, error))
    return FALSE;

  g_autoptr (GFile) save_dir = g_file_new_for_path (save_path);
  g_autoptr (GFile) save_file = g_file_get_child (save_dir, "save.sav");
  if (g_file_query_exists (save_file, NULL)) {
    g_autofree char *save_data = NULL;
    gsize save_length = 0;

    if (!g_file_load_contents (save_file, NULL, &save_data, &save_length, NULL, error))
      return FALSE;

    self->console->GetNDSCart ()->SetSaveMemory ((const u8*) save_data, save_length);
  }

  g_set_str (&self->save_path, g_file_get_path (save_file));

  return TRUE;
}

static void
melonds_core_load_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  melonDSCore *self = MELONDS_CORE (core);
  g_autofree char *data = NULL;
  gsize length;
  GError *error = NULL;

  if (!g_file_get_contents (path, &data, &length, &error)) {
    callback (core, &error);
    return;
  }

  Savestate *state = new Savestate (data, length, false);

  if (!self->console->DoSavestate (state) || state->Error) {
    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load state");
    callback (core, &error);
    return;
  }

  /* The first couple frames will be bad, skip them */
  self->skip_frames = N_BAD_FRAMES;

  delete state;
  callback (core, NULL);
}

static void
melonds_core_save_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  melonDSCore *self = MELONDS_CORE (core);
  g_autoptr (GFile) file = g_file_new_for_path (path);
  GError *error = NULL;

  Savestate state (Savestate::DEFAULT_SIZE);

  if (!self->console->DoSavestate (&state) || state.Error) {
    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to save state");
    callback (core, &error);
    return;
  }

  if (!g_file_replace_contents (file, (char*) state.Buffer (), state.Length (), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &error)) {
    callback (core, &error);
    return;
  }

  callback (core, NULL);
}

static double
melonds_core_get_frame_rate (HsCore *core)
{
  return 60;
}

static double
melonds_core_get_aspect_ratio (HsCore *core)
{
  return SCREEN_WIDTH / (double) SCREEN_HEIGHT / 2;
}

static double
melonds_core_get_sample_rate (HsCore *core)
{
  return SAMPLE_RATE;
}

static void
melonds_core_finalize (GObject *object)
{
  core = NULL;

  G_OBJECT_CLASS (melonds_core_parent_class)->finalize (object);
}

static void
melonds_core_class_init (melonDSCoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  HsCoreClass *core_class = HS_CORE_CLASS (klass);

  object_class->finalize = melonds_core_finalize;

  core_class->load_rom = melonds_core_load_rom;
  core_class->start = melonds_core_start;
  core_class->reset = melonds_core_reset;
  core_class->stop = melonds_core_stop;
  core_class->poll_input = melonds_core_poll_input;
  core_class->run_frame = melonds_core_run_frame;

  core_class->reload_save = melonds_core_reload_save;

  core_class->load_state = melonds_core_load_state;
  core_class->save_state = melonds_core_save_state;

  core_class->get_frame_rate = melonds_core_get_frame_rate;
  core_class->get_aspect_ratio = melonds_core_get_aspect_ratio;

  core_class->get_sample_rate = melonds_core_get_sample_rate;
}

static void
melonds_core_init (melonDSCore *self)
{
  g_assert (core == NULL);

  core = HS_CORE (self);
}

static void
melonds_nintendo_ds_core_init (HsNintendoDsCoreInterface *iface)
{
}

void
melonds_core_log (HsLogLevel level, const char *message)
{
  hs_core_log_literal (core, level, message);
}

const char *
melonds_core_get_save_path (void)
{
  return MELONDS_CORE (core)->save_path;
}

const char *
melonds_core_get_cache_path (void)
{
  return hs_core_get_cache_path (core);
}

gboolean
melonds_core_get_mic_active (void)
{
  return MELONDS_CORE (core)->mic_active;
}

GType
hs_get_core_type (void)
{
  return MELONDS_TYPE_CORE;
}
