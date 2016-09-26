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
#ifndef EGL_H_
#define EGL_H_

#include <common/messages.h>

#include "red-parse-qxl.h"

/* context attached to every SpiceDrmPrime */
struct SpiceDrmPrimeContext {
    void *egl_display;
    void *egl_context;
    /* Texture use for extraction.
     * This texture will be allocated when needed.
     */
    unsigned int tex_id;
};

/* Extract data from DRM prime and convert to normal bitmap.
 * If image does not contain a DRM prime nothing is changed.
 */
void image_extract_drm(SpiceImage *image);

/* Similar to image_extract_drm but handle images inside a RedDrawable */
void red_drawable_extract_drm(RedDrawable *red_drawable);

void *get_scanout_raw_data(SpiceDrmPrime *scanout, size_t *data_size);

#endif /* EGL_H_ */
