/*
 * GStreamer
 * Copyright (C) 2012-2014 Matthew Waters <ystree00@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include "gl.h"
#include "gstglupload.h"

#if GST_GL_HAVE_PLATFORM_EGL
#include "egl/gsteglimagememory.h"
#endif

/**
 * SECTION:gstglupload
 * @short_description: an object that uploads to GL textures
 * @see_also: #GstGLDownload, #GstGLMemory
 *
 * #GstGLUpload is an object that uploads data from system memory into GL textures.
 *
 * A #GstGLUpload can be created with gst_gl_upload_new()
 */

#define USING_OPENGL(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL, 1, 0))
#define USING_OPENGL3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL3, 3, 1))
#define USING_GLES(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES, 1, 0))
#define USING_GLES2(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 2, 0))
#define USING_GLES3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 3, 0))

GST_DEBUG_CATEGORY_STATIC (gst_gl_upload_debug);
#define GST_CAT_DEFAULT gst_gl_upload_debug

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_upload_debug, "glupload", 0, "upload");

G_DEFINE_TYPE_WITH_CODE (GstGLUpload, gst_gl_upload, GST_TYPE_OBJECT,
    DEBUG_INIT);
static void gst_gl_upload_finalize (GObject * object);

#define GST_GL_UPLOAD_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    GST_TYPE_GL_UPLOAD, GstGLUploadPrivate))

static GstGLTextureTarget
_caps_get_texture_target (GstCaps * caps, GstGLTextureTarget default_target)
{
  GstGLTextureTarget ret = 0;
  GstStructure *s = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_field_typed (s, "texture-target", G_TYPE_STRING)) {
    const gchar *target_str = gst_structure_get_string (s, "texture-target");
    ret = gst_gl_texture_target_from_string (target_str);
  }

  if (!ret)
    ret = default_target;

  return ret;
}

/* Define the maximum number of planes we can upload - handle 2 views per buffer */
#define GST_GL_UPLOAD_MAX_PLANES (GST_VIDEO_MAX_PLANES * 2)

typedef struct _UploadMethod UploadMethod;

struct _GstGLUploadPrivate
{
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  GstCaps *in_caps;
  GstCaps *out_caps;

  GstBuffer *outbuf;

  /* all method impl pointers */
  gpointer *upload_impl;

  /* current method */
  const UploadMethod *method;
  gpointer method_impl;
  int method_i;
};

static GstCaps *
_set_caps_features (const GstCaps * caps, const gchar * feature_name)
{
  GstCaps *tmp = gst_caps_copy (caps);
  guint n = gst_caps_get_size (tmp);
  guint i = 0;

  for (i = 0; i < n; i++) {
    GstCapsFeatures *features;

    features = gst_caps_features_new (feature_name, NULL);
    gst_caps_set_features (tmp, i, features);
  }

  return tmp;
}

typedef enum
{
  METHOD_FLAG_CAN_SHARE_CONTEXT = 1,
} GstGLUploadMethodFlags;

struct _UploadMethod
{
  const gchar *name;
  GstGLUploadMethodFlags flags;

  GstStaticCaps *input_template_caps;

    gpointer (*new) (GstGLUpload * upload);
  GstCaps *(*transform_caps) (GstGLContext * context,
      GstPadDirection direction, GstCaps * caps);
    gboolean (*accept) (gpointer impl, GstBuffer * buffer, GstCaps * in_caps,
      GstCaps * out_caps);
  void (*propose_allocation) (gpointer impl, GstQuery * decide_query,
      GstQuery * query);
    GstGLUploadReturn (*perform) (gpointer impl, GstBuffer * buffer,
      GstBuffer ** outbuf);
  void (*free) (gpointer impl);
} _UploadMethod;

struct GLMemoryUpload
{
  GstGLUpload *upload;
};

static gpointer
_gl_memory_upload_new (GstGLUpload * upload)
{
  struct GLMemoryUpload *mem = g_new0 (struct GLMemoryUpload, 1);

  mem->upload = upload;

  return mem;
}

static GstCaps *
_gl_memory_upload_transform_caps (GstGLContext * context,
    GstPadDirection direction, GstCaps * caps)
{
  return _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
}

static gboolean
_gl_memory_upload_accept (gpointer impl, GstBuffer * buffer, GstCaps * in_caps,
    GstCaps * out_caps)
{
  struct GLMemoryUpload *upload = impl;
  GstCapsFeatures *features;
  int i;

  features = gst_caps_get_features (out_caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY))
    return FALSE;

  features = gst_caps_get_features (in_caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY)
      && !gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY))
    return FALSE;

  if (buffer) {
    GstVideoInfo *in_info = &upload->upload->priv->in_info;
    guint expected_memories = GST_VIDEO_INFO_N_PLANES (in_info);

    /* Support stereo views for separated multiview mode */
    if (GST_VIDEO_INFO_MULTIVIEW_MODE (in_info) ==
        GST_VIDEO_MULTIVIEW_MODE_SEPARATED)
      expected_memories *= GST_VIDEO_INFO_VIEWS (in_info);

    if (gst_buffer_n_memory (buffer) != expected_memories)
      return FALSE;

    for (i = 0; i < expected_memories; i++) {
      GstMemory *mem = gst_buffer_peek_memory (buffer, i);

      if (!gst_is_gl_memory (mem))
        return FALSE;
    }
  }

  return TRUE;
}

static void
_gl_memory_upload_propose_allocation (gpointer impl, GstQuery * decide_query,
    GstQuery * query)
{
  struct GLMemoryUpload *upload = impl;
  GstAllocationParams params;
  GstAllocator *allocator;
  GstBufferPool *pool = NULL;
  guint n_pools, i;

  gst_allocation_params_init (&params);

  allocator = gst_allocator_find (GST_GL_MEMORY_ALLOCATOR);
  gst_query_add_allocation_param (query, allocator, &params);
  gst_object_unref (allocator);

  n_pools = gst_query_get_n_allocation_pools (query);
  for (i = 0; i < n_pools; i++) {
    gst_query_parse_nth_allocation_pool (query, i, &pool, NULL, NULL, NULL);
    if (!GST_IS_GL_BUFFER_POOL (pool)) {
      gst_object_unref (pool);
      pool = NULL;
    }
  }

  if (!pool) {
    GstStructure *config;
    GstVideoInfo info;
    GstCaps *caps;
    gsize size;

    gst_query_parse_allocation (query, &caps, NULL);

    if (!gst_video_info_from_caps (&info, caps))
      goto invalid_caps;

    pool = gst_gl_buffer_pool_new (upload->upload->context);
    config = gst_buffer_pool_get_config (pool);

    /* the normal size of a frame */
    size = info.size;
    gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_GL_SYNC_META);
    if (upload->upload->priv->out_caps) {
      GstGLTextureTarget target;
      const gchar *target_pool_option_str;

      target =
          _caps_get_texture_target (upload->upload->priv->out_caps,
          GST_GL_TEXTURE_TARGET_2D);
      target_pool_option_str =
          gst_gl_texture_target_to_buffer_pool_option (target);
      gst_buffer_pool_config_add_option (config, target_pool_option_str);
    }

    if (!gst_buffer_pool_set_config (pool, config)) {
      gst_object_unref (pool);
      goto config_failed;
    }

    gst_query_add_allocation_pool (query, pool, size, 1, 0);
  }

  if (pool)
    gst_object_unref (pool);

  return;

invalid_caps:
  {
    GST_WARNING_OBJECT (upload->upload, "invalid caps specified");
    return;
  }
config_failed:
  {
    GST_WARNING_OBJECT (upload->upload, "failed setting config");
    return;
  }
}

static GstGLUploadReturn
_gl_memory_upload_perform (gpointer impl, GstBuffer * buffer,
    GstBuffer ** outbuf)
{
  struct GLMemoryUpload *upload = impl;
  GstGLMemory *gl_mem;
  int i, n;

  n = gst_buffer_n_memory (buffer);
  for (i = 0; i < n; i++) {
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);

    gl_mem = (GstGLMemory *) mem;
    if (!gst_gl_context_can_share (upload->upload->context,
            gl_mem->mem.context))
      return GST_GL_UPLOAD_UNSHARED_GL_CONTEXT;

    gst_gl_memory_upload_transfer (gl_mem);
  }

  *outbuf = gst_buffer_ref (buffer);

  return GST_GL_UPLOAD_DONE;
}

static void
_gl_memory_upload_free (gpointer impl)
{
  g_free (impl);
}


static GstStaticCaps _gl_memory_upload_caps =
GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, GST_GL_MEMORY_VIDEO_FORMATS_STR));

static const UploadMethod _gl_memory_upload = {
  "GLMemory",
  METHOD_FLAG_CAN_SHARE_CONTEXT,
  &_gl_memory_upload_caps,
  &_gl_memory_upload_new,
  &_gl_memory_upload_transform_caps,
  &_gl_memory_upload_accept,
  &_gl_memory_upload_propose_allocation,
  &_gl_memory_upload_perform,
  &_gl_memory_upload_free
};

#if GST_GL_HAVE_PLATFORM_EGL
struct EGLImageUpload
{
  GstGLUpload *upload;
  GstBuffer *buffer;
  GstBuffer **outbuf;
};

static gpointer
_egl_image_upload_new (GstGLUpload * upload)
{
  struct EGLImageUpload *image = g_new0 (struct EGLImageUpload, 1);

  image->upload = upload;

  return image;
}

static GstCaps *
_egl_image_upload_transform_caps (GstGLContext * context,
    GstPadDirection direction, GstCaps * caps)
{
  GstCaps *ret;

  if (direction == GST_PAD_SINK) {
    ret = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
  } else {
    gint i, n;

    ret = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_EGL_IMAGE);
    gst_caps_set_simple (ret, "format", G_TYPE_STRING, "RGBA", NULL);

    n = gst_caps_get_size (ret);
    for (i = 0; i < n; i++) {
      GstStructure *s = gst_caps_get_structure (ret, i);

      gst_structure_remove_fields (s, "texture-target", NULL);
    }
  }

  return ret;
}

static gboolean
_egl_image_upload_accept (gpointer impl, GstBuffer * buffer, GstCaps * in_caps,
    GstCaps * out_caps)
{
  struct EGLImageUpload *image = impl;
  GstCapsFeatures *features;
  gboolean ret = TRUE;
  int i;

  features = gst_caps_get_features (in_caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_EGL_IMAGE))
    ret = FALSE;

  features = gst_caps_get_features (out_caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY))
    ret = FALSE;

  if (!ret)
    return FALSE;

  if (buffer) {
    GstVideoInfo *in_info = &image->upload->priv->in_info;
    guint expected_memories = GST_VIDEO_INFO_N_PLANES (in_info);

    /* Support stereo views for separated multiview mode */
    if (GST_VIDEO_INFO_MULTIVIEW_MODE (in_info) ==
        GST_VIDEO_MULTIVIEW_MODE_SEPARATED)
      expected_memories *= GST_VIDEO_INFO_VIEWS (in_info);

    if (gst_buffer_n_memory (buffer) != expected_memories)
      return FALSE;

    for (i = 0; i < expected_memories; i++) {
      GstMemory *mem = gst_buffer_peek_memory (buffer, i);

      if (!gst_is_egl_image_memory (mem))
        return FALSE;
    }
  }

  return TRUE;
}

static void
_egl_image_upload_propose_allocation (gpointer impl, GstQuery * decide_query,
    GstQuery * query)
{
  struct EGLImageUpload *image = impl;
  GstAllocationParams params;
  GstAllocator *allocator;

  gst_allocation_params_init (&params);

  if (gst_gl_context_check_feature (image->upload->context,
          "EGL_KHR_image_base")) {
    allocator = gst_allocator_find (GST_EGL_IMAGE_MEMORY_TYPE);
    gst_query_add_allocation_param (query, allocator, &params);
    gst_object_unref (allocator);
  }
}

static void
_egl_image_upload_perform_gl_thread (GstGLContext * context,
    struct EGLImageUpload *image)
{
  guint i, n;

  /* FIXME: buffer pool */
  *image->outbuf = gst_buffer_new ();
  gst_gl_memory_setup_buffer (image->upload->context, GST_GL_TEXTURE_TARGET_2D,
      NULL, &image->upload->priv->out_info, NULL, *image->outbuf);

  n = gst_buffer_n_memory (image->buffer);
  for (i = 0; i < n; i++) {
    GstMemory *mem = gst_buffer_peek_memory (image->buffer, i);
    GstGLMemory *out_gl_mem =
        (GstGLMemory *) gst_buffer_peek_memory (*image->outbuf, i);
    const GstGLFuncs *gl = NULL;

    gl = GST_GL_CONTEXT (((GstEGLImageMemory *) mem)->context)->gl_vtable;

    gl->ActiveTexture (GL_TEXTURE0 + i);
    gl->BindTexture (GL_TEXTURE_2D, out_gl_mem->tex_id);
    gl->EGLImageTargetTexture2D (GL_TEXTURE_2D,
        gst_egl_image_memory_get_image (mem));
  }

  if (GST_IS_GL_BUFFER_POOL (image->buffer->pool))
    gst_gl_buffer_pool_replace_last_buffer (GST_GL_BUFFER_POOL (image->buffer->
            pool), image->buffer);
}

static GstGLUploadReturn
_egl_image_upload_perform (gpointer impl, GstBuffer * buffer,
    GstBuffer ** outbuf)
{
  struct EGLImageUpload *image = impl;

  image->buffer = buffer;
  image->outbuf = outbuf;

  gst_gl_context_thread_add (image->upload->context,
      (GstGLContextThreadFunc) _egl_image_upload_perform_gl_thread, image);

  if (!*image->outbuf)
    return GST_GL_UPLOAD_ERROR;

  return GST_GL_UPLOAD_DONE;
}

static void
_egl_image_upload_free (gpointer impl)
{
  g_free (impl);
}

static GstStaticCaps _egl_image_upload_caps =
GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_EGL_IMAGE, "RGBA"));

static const UploadMethod _egl_image_upload = {
  "EGLImage",
  0,
  &_egl_image_upload_caps,
  &_egl_image_upload_new,
  &_egl_image_upload_transform_caps,
  &_egl_image_upload_accept,
  &_egl_image_upload_propose_allocation,
  &_egl_image_upload_perform,
  &_egl_image_upload_free
};
#endif

struct GLUploadMeta
{
  GstGLUpload *upload;

  gboolean result;
  GstVideoGLTextureUploadMeta *meta;
  guint texture_ids[GST_GL_UPLOAD_MAX_PLANES];
};

static gpointer
_upload_meta_upload_new (GstGLUpload * upload)
{
  struct GLUploadMeta *meta = g_new0 (struct GLUploadMeta, 1);

  meta->upload = upload;

  return meta;
}

static GstCaps *
_upload_meta_upload_transform_caps (GstGLContext * context,
    GstPadDirection direction, GstCaps * caps)
{
  GstCaps *ret;

  if (direction == GST_PAD_SINK) {
    ret = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
  } else {
    gint i, n;

    ret =
        _set_caps_features (caps,
        GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META);
    gst_caps_set_simple (ret, "format", G_TYPE_STRING, "RGBA", NULL);

    n = gst_caps_get_size (ret);
    for (i = 0; i < n; i++) {
      GstStructure *s = gst_caps_get_structure (ret, i);

      gst_structure_remove_fields (s, "texture-target", NULL);
    }
  }

  return ret;
}

static gboolean
_upload_meta_upload_accept (gpointer impl, GstBuffer * buffer,
    GstCaps * in_caps, GstCaps * out_caps)
{
  struct GLUploadMeta *upload = impl;
  GstCapsFeatures *features;
  GstVideoGLTextureUploadMeta *meta;
  gboolean ret = TRUE;

  features = gst_caps_get_features (in_caps, 0);

  if (!gst_caps_features_contains (features,
          GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META))
    ret = FALSE;

  features = gst_caps_get_features (out_caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY))
    ret = FALSE;

  if (!ret)
    return ret;

  if (buffer) {
    if ((meta = gst_buffer_get_video_gl_texture_upload_meta (buffer)) == NULL)
      return FALSE;

    if (meta->texture_type[0] != GST_VIDEO_GL_TEXTURE_TYPE_RGBA) {
      GST_FIXME_OBJECT (upload, "only single rgba texture supported");
      return FALSE;
    }

    if (meta->texture_orientation !=
        GST_VIDEO_GL_TEXTURE_ORIENTATION_X_NORMAL_Y_NORMAL) {
      GST_FIXME_OBJECT (upload, "only x-normal, y-normal textures supported");
      return FALSE;
    }
  }

  return TRUE;
}

static void
_upload_meta_upload_propose_allocation (gpointer impl, GstQuery * decide_query,
    GstQuery * query)
{
  struct GLUploadMeta *upload = impl;
  GstStructure *gl_context;
  gchar *platform, *gl_apis;
  gpointer handle;

  gl_apis =
      gst_gl_api_to_string (gst_gl_context_get_gl_api (upload->
          upload->context));
  platform =
      gst_gl_platform_to_string (gst_gl_context_get_gl_platform
      (upload->upload->context));
  handle = (gpointer) gst_gl_context_get_gl_context (upload->upload->context);

  gl_context =
      gst_structure_new ("GstVideoGLTextureUploadMeta", "gst.gl.GstGLContext",
      GST_GL_TYPE_CONTEXT, upload->upload->context, "gst.gl.context.handle",
      G_TYPE_POINTER, handle, "gst.gl.context.type", G_TYPE_STRING, platform,
      "gst.gl.context.apis", G_TYPE_STRING, gl_apis, NULL);
  gst_query_add_allocation_meta (query,
      GST_VIDEO_GL_TEXTURE_UPLOAD_META_API_TYPE, gl_context);

  g_free (gl_apis);
  g_free (platform);
  gst_structure_free (gl_context);
}

/*
 * Uploads using gst_video_gl_texture_upload_meta_upload().
 * i.e. consumer of GstVideoGLTextureUploadMeta
 */
static void
_do_upload_with_meta (GstGLContext * context, struct GLUploadMeta *upload)
{
  if (!gst_video_gl_texture_upload_meta_upload (upload->meta,
          upload->texture_ids)) {
    upload->result = FALSE;
    return;
  }

  upload->result = TRUE;
}

static GstGLUploadReturn
_upload_meta_upload_perform (gpointer impl, GstBuffer * buffer,
    GstBuffer ** outbuf)
{
  struct GLUploadMeta *upload = impl;
  int i;
  GstVideoInfo *in_info = &upload->upload->priv->in_info;
  guint max_planes = GST_VIDEO_INFO_N_PLANES (in_info);

  /* Support stereo views for separated multiview mode */
  if (GST_VIDEO_INFO_MULTIVIEW_MODE (in_info) ==
      GST_VIDEO_MULTIVIEW_MODE_SEPARATED)
    max_planes *= GST_VIDEO_INFO_VIEWS (in_info);

  GST_LOG_OBJECT (upload, "Attempting upload with GstVideoGLTextureUploadMeta");

  upload->meta = gst_buffer_get_video_gl_texture_upload_meta (buffer);

  /* FIXME: buffer pool */
  *outbuf = gst_buffer_new ();
  gst_gl_memory_setup_buffer (upload->upload->context, GST_GL_TEXTURE_TARGET_2D,
      NULL, &upload->upload->priv->in_info, NULL, *outbuf);

  for (i = 0; i < GST_GL_UPLOAD_MAX_PLANES; i++) {
    guint tex_id = 0;

    if (i < max_planes) {
      GstMemory *mem = gst_buffer_peek_memory (*outbuf, i);
      tex_id = ((GstGLMemory *) mem)->tex_id;
    }

    upload->texture_ids[i] = tex_id;
  }

  GST_LOG ("Uploading with GLTextureUploadMeta with textures "
      "%i,%i,%i,%i / %i,%i,%i,%i",
      upload->texture_ids[0], upload->texture_ids[1],
      upload->texture_ids[2], upload->texture_ids[3],
      upload->texture_ids[4], upload->texture_ids[5],
      upload->texture_ids[6], upload->texture_ids[7]);

  gst_gl_context_thread_add (upload->upload->context,
      (GstGLContextThreadFunc) _do_upload_with_meta, upload);

  if (!upload->result)
    return GST_GL_UPLOAD_ERROR;

  return GST_GL_UPLOAD_DONE;
}

static void
_upload_meta_upload_free (gpointer impl)
{
  struct GLUploadMeta *upload = impl;
  gint i;

  g_return_if_fail (impl != NULL);

  for (i = 0; i < GST_GL_UPLOAD_MAX_PLANES; i++) {
    if (upload->texture_ids[i])
      gst_gl_context_del_texture (upload->upload->context,
          &upload->texture_ids[i]);
  }
  g_free (upload);
}

static GstStaticCaps _upload_meta_upload_caps =
GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META, "RGBA"));

static const UploadMethod _upload_meta_upload = {
  "UploadMeta",
  METHOD_FLAG_CAN_SHARE_CONTEXT,
  &_upload_meta_upload_caps,
  &_upload_meta_upload_new,
  &_upload_meta_upload_transform_caps,
  &_upload_meta_upload_accept,
  &_upload_meta_upload_propose_allocation,
  &_upload_meta_upload_perform,
  &_upload_meta_upload_free
};

struct RawUploadFrame
{
  gint ref_count;
  GstVideoFrame frame;
};

struct RawUpload
{
  GstGLUpload *upload;
  struct RawUploadFrame *in_frame;
};

static struct RawUploadFrame *
_raw_upload_frame_new (struct RawUpload *raw, GstBuffer * buffer)
{
  struct RawUploadFrame *frame;
  GstVideoInfo *info;
  gint i;

  if (!buffer)
    return NULL;

  frame = g_slice_new (struct RawUploadFrame);
  frame->ref_count = 1;

  if (!gst_video_frame_map (&frame->frame, &raw->upload->priv->in_info,
          buffer, GST_MAP_READ)) {
    g_slice_free (struct RawUploadFrame, frame);
    return NULL;
  }

  raw->upload->priv->in_info = frame->frame.info;
  info = &raw->upload->priv->in_info;

  /* Recalculate the offsets (and size) */
  info->size = 0;
  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
    info->offset[i] = info->size;
    info->size += gst_gl_get_plane_data_size (info, NULL, i);
  }

  return frame;
}

static void
_raw_upload_frame_ref (struct RawUploadFrame *frame)
{
  g_atomic_int_inc (&frame->ref_count);
}

static void
_raw_upload_frame_unref (struct RawUploadFrame *frame)
{
  if (g_atomic_int_dec_and_test (&frame->ref_count)) {
    gst_video_frame_unmap (&frame->frame);
    g_slice_free (struct RawUploadFrame, frame);
  }
}

static gpointer
_raw_data_upload_new (GstGLUpload * upload)
{
  struct RawUpload *raw = g_new0 (struct RawUpload, 1);

  raw->upload = upload;

  return raw;
}

static GstCaps *
_raw_data_upload_transform_caps (GstGLContext * context,
    GstPadDirection direction, GstCaps * caps)
{
  GstCaps *ret;

  if (direction == GST_PAD_SINK) {
    ret = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
  } else {
    gint i, n;

    ret = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY);

    n = gst_caps_get_size (ret);
    for (i = 0; i < n; i++) {
      GstStructure *s = gst_caps_get_structure (ret, i);

      gst_structure_remove_fields (s, "texture-target", NULL);
    }
  }

  return ret;
}

static gboolean
_raw_data_upload_accept (gpointer impl, GstBuffer * buffer, GstCaps * in_caps,
    GstCaps * out_caps)
{
  struct RawUpload *raw = impl;
  GstCapsFeatures *features;

  features = gst_caps_get_features (out_caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY))
    return FALSE;

  if (raw->in_frame)
    _raw_upload_frame_unref (raw->in_frame);
  raw->in_frame = _raw_upload_frame_new (raw, buffer);

  return (raw->in_frame != NULL);
}

static void
_raw_data_upload_propose_allocation (gpointer impl, GstQuery * decide_query,
    GstQuery * query)
{
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, 0);
}

static GstGLUploadReturn
_raw_data_upload_perform (gpointer impl, GstBuffer * buffer,
    GstBuffer ** outbuf)
{
  GstGLMemory *in_tex[GST_GL_UPLOAD_MAX_PLANES] = { 0, };
  struct RawUpload *raw = impl;
  int i;
  GstVideoInfo *in_info = &raw->upload->priv->in_info;
  guint max_planes = GST_VIDEO_INFO_N_PLANES (in_info);

  /* Support stereo views for separated multiview mode */
  if (GST_VIDEO_INFO_MULTIVIEW_MODE (in_info) ==
      GST_VIDEO_MULTIVIEW_MODE_SEPARATED)
    max_planes *= GST_VIDEO_INFO_VIEWS (in_info);

  gst_gl_memory_setup_wrapped (raw->upload->context, GST_GL_TEXTURE_TARGET_2D,
      &raw->upload->priv->in_info, NULL, raw->in_frame->frame.data, in_tex,
      raw->in_frame, (GDestroyNotify) _raw_upload_frame_unref);

  /* FIXME Use a buffer pool to cache the generated textures */
  *outbuf = gst_buffer_new ();
  for (i = 0; i < max_planes; i++) {
    _raw_upload_frame_ref (raw->in_frame);
    gst_buffer_append_memory (*outbuf, (GstMemory *) in_tex[i]);
  }

  _raw_upload_frame_unref (raw->in_frame);
  raw->in_frame = NULL;
  return GST_GL_UPLOAD_DONE;
}

static void
_raw_data_upload_free (gpointer impl)
{
  struct RawUpload *raw = impl;

  g_free (raw);
}

static GstStaticCaps _raw_data_upload_caps =
GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (GST_GL_MEMORY_VIDEO_FORMATS_STR));

static const UploadMethod _raw_data_upload = {
  "Raw Data",
  0,
  &_raw_data_upload_caps,
  &_raw_data_upload_new,
  &_raw_data_upload_transform_caps,
  &_raw_data_upload_accept,
  &_raw_data_upload_propose_allocation,
  &_raw_data_upload_perform,
  &_raw_data_upload_free
};

static const UploadMethod *upload_methods[] = { &_gl_memory_upload,
#if GST_GL_HAVE_PLATFORM_EGL
  &_egl_image_upload,
#endif
  &_upload_meta_upload, &_raw_data_upload
};

static GMutex upload_global_lock;

GstCaps *
gst_gl_upload_get_input_template_caps (void)
{
  GstCaps *ret = NULL;
  gint i;

  g_mutex_lock (&upload_global_lock);

  /* FIXME: cache this and invalidate on changes to upload_methods */
  for (i = 0; i < G_N_ELEMENTS (upload_methods); i++) {
    GstCaps *template =
        gst_static_caps_get (upload_methods[i]->input_template_caps);
    ret = ret == NULL ? template : gst_caps_merge (ret, template);
  }

  ret = gst_caps_simplify (ret);
  ret = gst_gl_overlay_compositor_add_caps (ret);
  g_mutex_unlock (&upload_global_lock);

  return ret;
}

static void
gst_gl_upload_class_init (GstGLUploadClass * klass)
{
  g_type_class_add_private (klass, sizeof (GstGLUploadPrivate));

  G_OBJECT_CLASS (klass)->finalize = gst_gl_upload_finalize;
}

static void
gst_gl_upload_init (GstGLUpload * upload)
{
  upload->priv = GST_GL_UPLOAD_GET_PRIVATE (upload);
}

/**
 * gst_gl_upload_new:
 * @context: a #GstGLContext
 *
 * Returns: a new #GstGLUpload object
 */
GstGLUpload *
gst_gl_upload_new (GstGLContext * context)
{
  GstGLUpload *upload = g_object_new (GST_TYPE_GL_UPLOAD, NULL);
  gint i, n;

  upload->context = gst_object_ref (context);

  n = G_N_ELEMENTS (upload_methods);
  upload->priv->upload_impl = g_malloc (sizeof (gpointer) * n);
  for (i = 0; i < n; i++) {
    upload->priv->upload_impl[i] = upload_methods[i]->new (upload);
  }

  GST_DEBUG_OBJECT (upload, "Created new GLUpload for context %" GST_PTR_FORMAT,
      context);

  return upload;
}

static void
gst_gl_upload_finalize (GObject * object)
{
  GstGLUpload *upload;
  gint i, n;

  upload = GST_GL_UPLOAD (object);

  if (upload->priv->method_impl)
    upload->priv->method->free (upload->priv->method_impl);
  upload->priv->method_i = 0;

  if (upload->context) {
    gst_object_unref (upload->context);
    upload->context = NULL;
  }

  if (upload->priv->in_caps) {
    gst_caps_unref (upload->priv->in_caps);
    upload->priv->in_caps = NULL;
  }

  if (upload->priv->out_caps) {
    gst_caps_unref (upload->priv->out_caps);
    upload->priv->out_caps = NULL;
  }

  n = G_N_ELEMENTS (upload_methods);
  for (i = 0; i < n; i++) {
    if (upload->priv->upload_impl[i])
      upload_methods[i]->free (upload->priv->upload_impl[i]);
  }
  g_free (upload->priv->upload_impl);

  G_OBJECT_CLASS (gst_gl_upload_parent_class)->finalize (object);
}

GstCaps *
gst_gl_upload_transform_caps (GstGLContext * context, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstCaps *result, *tmp;
  gint i;

  tmp = gst_caps_new_empty ();

  for (i = 0; i < G_N_ELEMENTS (upload_methods); i++) {
    GstCaps *tmp2;

    tmp2 = upload_methods[i]->transform_caps (context, direction, caps);

    if (tmp2)
      tmp = gst_caps_merge (tmp, tmp2);
  }

  tmp = gst_gl_overlay_compositor_add_caps (tmp);


  if (filter) {
    result = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp);
  } else {
    result = tmp;
  }

  return result;
}

/**
 * gst_gl_upload_propose_allocation:
 * @upload: a #GstGLUpload
 * @decide_query: (allow-none): a #GstQuery from a decide allocation
 * @query: the proposed allocation query
 *
 * Adds the required allocation parameters to support uploading.
 */
void
gst_gl_upload_propose_allocation (GstGLUpload * upload, GstQuery * decide_query,
    GstQuery * query)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (upload_methods); i++)
    upload_methods[i]->propose_allocation (upload->priv->upload_impl[i],
        decide_query, query);
}

static gboolean
_gst_gl_upload_set_caps_unlocked (GstGLUpload * upload, GstCaps * in_caps,
    GstCaps * out_caps)
{
  g_return_val_if_fail (upload != NULL, FALSE);
  g_return_val_if_fail (gst_caps_is_fixed (in_caps), FALSE);

  if (upload->priv->in_caps && upload->priv->out_caps
      && gst_caps_is_equal (upload->priv->in_caps, in_caps)
      && gst_caps_is_equal (upload->priv->out_caps, out_caps))
    return TRUE;

  gst_caps_replace (&upload->priv->in_caps, in_caps);
  gst_caps_replace (&upload->priv->out_caps, out_caps);

  gst_video_info_from_caps (&upload->priv->in_info, in_caps);
  gst_video_info_from_caps (&upload->priv->out_info, out_caps);

  if (upload->priv->method_impl)
    upload->priv->method->free (upload->priv->method_impl);
  upload->priv->method_impl = NULL;
  upload->priv->method_i = 0;

  return TRUE;
}

/**
 * gst_gl_upload_set_caps:
 * @upload: a #GstGLUpload
 * @in_caps: input #GstCaps
 * @out_caps: output #GstCaps
 *
 * Initializes @upload with the information required for upload.
 *
 * Returns: whether @in_caps and @out_caps could be set on @upload
 */
gboolean
gst_gl_upload_set_caps (GstGLUpload * upload, GstCaps * in_caps,
    GstCaps * out_caps)
{
  gboolean ret;

  GST_OBJECT_LOCK (upload);
  ret = _gst_gl_upload_set_caps_unlocked (upload, in_caps, out_caps);
  GST_OBJECT_UNLOCK (upload);

  return ret;
}

/**
 * gst_gl_upload_get_caps:
 * @upload: a #GstGLUpload
 * @in_caps: (transfer full) (allow-none) (out): the input #GstCaps
 * @out_caps: (transfer full) (allow-none) (out): the output #GstCaps
 *
 * Returns: (transfer none): The #GstCaps set by gst_gl_upload_set_caps()
 */
void
gst_gl_upload_get_caps (GstGLUpload * upload, GstCaps ** in_caps,
    GstCaps ** out_caps)
{
  GST_OBJECT_LOCK (upload);
  if (in_caps)
    *in_caps =
        upload->priv->in_caps ? gst_caps_ref (upload->priv->in_caps) : NULL;
  if (out_caps)
    *out_caps =
        upload->priv->out_caps ? gst_caps_ref (upload->priv->out_caps) : NULL;
  GST_OBJECT_UNLOCK (upload);
}

static gboolean
_upload_find_method (GstGLUpload * upload)
{
  if (upload->priv->method_i >= G_N_ELEMENTS (upload_methods))
    return FALSE;

  if (upload->priv->method_impl) {
    upload->priv->method->free (upload->priv->method_impl);
    upload->priv->method_impl = NULL;
  }

  upload->priv->method = upload_methods[upload->priv->method_i];
  upload->priv->method_impl = upload->priv->method->new (upload);

  GST_DEBUG_OBJECT (upload, "attempting upload with uploader %s",
      upload->priv->method->name);

  upload->priv->method_i++;

  return TRUE;
}

/**
 * gst_gl_upload_perform_with_buffer:
 * @upload: a #GstGLUpload
 * @buffer: a #GstBuffer
 * @outbuf_ptr: esulting buffer
 *
 * Uploads @buffer using the transformation specified by
 * gst_gl_upload_set_caps().
 *
 * Returns: whether the upload was successful
 */
GstGLUploadReturn
gst_gl_upload_perform_with_buffer (GstGLUpload * upload, GstBuffer * buffer,
    GstBuffer ** outbuf_ptr)
{
  GstGLUploadReturn ret = GST_GL_UPLOAD_ERROR;
  GstBuffer *outbuf;

  g_return_val_if_fail (GST_IS_GL_UPLOAD (upload), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (buffer), FALSE);
  g_return_val_if_fail (outbuf_ptr != NULL, FALSE);

  GST_OBJECT_LOCK (upload);

#define NEXT_METHOD \
do { \
  if (!_upload_find_method (upload)) { \
    GST_OBJECT_UNLOCK (upload); \
    return FALSE; \
  } \
  goto restart; \
} while (0)

  if (!upload->priv->method_impl)
    _upload_find_method (upload);

restart:
  if (!upload->priv->method->accept (upload->priv->method_impl, buffer,
          upload->priv->in_caps, upload->priv->out_caps))
    NEXT_METHOD;

  ret =
      upload->priv->method->perform (upload->priv->method_impl, buffer,
      &outbuf);
  if (ret == GST_GL_UPLOAD_UNSHARED_GL_CONTEXT) {
    upload->priv->method->free (upload->priv->method_impl);
    upload->priv->method = &_raw_data_upload;
    upload->priv->method_impl = upload->priv->method->new (upload);
    goto restart;
  } else if (ret == GST_GL_UPLOAD_DONE) {
    /* we are done */
  } else {
    upload->priv->method->free (upload->priv->method_impl);
    upload->priv->method_impl = NULL;
    NEXT_METHOD;
  }

  if (buffer != outbuf)
    gst_buffer_copy_into (outbuf, buffer,
        GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
  *outbuf_ptr = outbuf;

  GST_OBJECT_UNLOCK (upload);

  return ret;

#undef NEXT_METHOD
}
