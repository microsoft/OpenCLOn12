/*
 * Copyright 2019 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef CLC_COMPILER_H
#define CLC_COMPILER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#if defined(BUILD_COMPILER)
   #include "util/macros.h"
#elif defined(_MSC_VER)
   #define PUBLIC __declspec(dllimport)
#elif !defined(PUBLIC)
   #define PUBLIC
#endif

struct clc_define {
   const char *name;
   const char *definition;
};

struct clc_header {
   const char *name;
   const char *source;
};

typedef void (*clc_msg_callback)(const char *, int, const char *);

#define CLC_MAX_CONST_ARGS 8
#define CLC_MAX_READ_IMAGE_ARGS 128
#define CLC_MAX_WRITE_IMAGE_ARGS 8

#define CLC_MAX_ARGS (CLC_MAX_CONST_ARGS + CLC_MAX_READ_IMAGE_ARGS + \
                      CLC_MAX_WRITE_IMAGE_ARGS)

struct clc_metadata {
   struct {
      enum {
         CLC_ARG_CONST,
         CLC_ARG_READ_IMAGE,
         CLC_ARG_WRITE_IMAGE
      } type;
      union {
         struct {
            /* TODO */
            int dummy;
         } const_arg;
         struct {
            /* TODO */
            int dummy;
         } image_arg;
      };
   } args[CLC_MAX_ARGS];
   size_t num_args;

   struct {
      int image_index;
      int cbuf_offset;
   } image_channels[CLC_MAX_READ_IMAGE_ARGS + CLC_MAX_WRITE_IMAGE_ARGS];
   size_t num_image_channels;
};

PUBLIC int clc_compile_from_source(
   const char *source,
   const char *source_name,
   const struct clc_define defines[], // should be sorted by name
   size_t num_defines,
   const struct clc_header headers[], // should be sorted by name
   size_t num_headers,
   clc_msg_callback warning_callback,
   clc_msg_callback error_callback,
   struct clc_metadata *metadata,
   void **blob,
   size_t *blob_size);

PUBLIC void free_blob(void* blob);

#ifdef __cplusplus
}
#endif

#endif
