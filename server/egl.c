/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2016 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include "egl.h"

static void *egl_get_context(void *egl_display);

void *get_scanout_raw_data(SpiceDrmPrime *scanout, size_t *data_size)
{
    EGLDisplay display = scanout->context->egl_display;

    spice_assert(display);

    if (!scanout->context->egl_context) {
        scanout->context->egl_context = egl_get_context(display);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       scanout->context->egl_context);
    }

    if (!scanout->context->tex_id) {
        glGenTextures(1, &scanout->context->tex_id);
    }
    GLuint tex_id = scanout->context->tex_id;

    // import file descriptor into an image
    EGLint attrs[13];
    attrs[0] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[1] = scanout->drm_dma_buf_fd;
    attrs[2] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[3] = scanout->stride;
    attrs[4] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[5] = 0;
    attrs[6] = EGL_WIDTH;
    attrs[7] = scanout->width;
    attrs[8] = EGL_HEIGHT;
    attrs[9] = scanout->height;
    attrs[10] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[11] = scanout->drm_fourcc_format;
    attrs[12] = EGL_NONE;
    EGLImageKHR image = eglCreateImageKHR(display,
                                          EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT,
                                          (EGLClientBuffer)NULL,
                                          attrs);

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);

    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w == 0 || h == 0) {
        eglDestroyImageKHR(display, image);
        return NULL;
    }
    size_t tex_size = w * h * 4;
    uint8_t *data = spice_malloc(tex_size);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
#if 0
    if (w > texture->width) {
        int y;
        for (y = 1; y < texture->height; ++y)
            memmove(data + texture->width * 4 * y, data + w * 4 * y, texture->width * 4);
        tex_size = texture->width * texture->height * 4;
    }
#endif

    eglDestroyImageKHR(display, image);

    *data_size = tex_size;
    return data;
}

void *egl_get_context(void *egl_display)
{
    static const EGLint ctx_att_gl[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_NONE
    };

    static const EGLint conf_att[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };

    EGLContext ectx;
    EGLBoolean b;
    EGLint n;
    EGLConfig egl_config;

    b = eglBindAPI(EGL_OPENGL_API);
    spice_assert(b);

    b = eglChooseConfig(egl_display, conf_att, &egl_config,
                        1, &n);
    spice_assert(b && n == 1);

    ectx = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT,
                            ctx_att_gl);
    spice_assert(ectx != EGL_NO_CONTEXT);

    return ectx;
}
