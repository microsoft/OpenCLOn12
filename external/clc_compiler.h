/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef CLC_COMPILER_H
#define CLC_COMPILER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

struct clc_named_value {
   const char *name;
   const char *value;
};

struct clc_compile_args {
   const struct clc_named_value *defines;
   unsigned num_defines;
   const struct clc_named_value *headers;
   unsigned num_headers;
   struct clc_named_value source;
};

typedef void (*clc_msg_callback)(const char *, int, const char *);

struct clc_logger {
   clc_msg_callback error;
   clc_msg_callback warning;
};

struct spirv_binary {
   uint32_t *data;
   size_t size;
};

enum clc_kernel_arg_type_qualifier {
   CLC_KERNEL_ARG_TYPE_CONST = 1 << 0,
   CLC_KERNEL_ARG_TYPE_RESTRICT = 1 << 1,
   CLC_KERNEL_ARG_TYPE_VOLATILE = 1 << 2,
};

enum clc_kernel_arg_access_qualifier {
   CLC_KERNEL_ARG_ACCESS_READ = 1 << 0,
   CLC_KERNEL_ARG_ACCESS_WRITE = 1 << 1,
};

enum clc_kernel_arg_address_qualifier {
   CLC_KERNEL_ARG_ADDRESS_PRIVATE,
   CLC_KERNEL_ARG_ADDRESS_CONSTANT,
   CLC_KERNEL_ARG_ADDRESS_LOCAL,
   CLC_KERNEL_ARG_ADDRESS_GLOBAL,
};

struct clc_kernel_arg {
   const char *name;
   const char *type_name;
   unsigned type_qualifier;
   unsigned access_qualifier;
   enum clc_kernel_arg_address_qualifier address_qualifier;
};

struct clc_kernel_info {
   const char *name;
   size_t num_args;
   const struct clc_kernel_arg *args;
};

struct clc_object {
   struct spirv_binary spvbin;
   const struct clc_kernel_info *kernels;
   unsigned num_kernels;
};

#define CLC_MAX_CONSTS 32
#define CLC_MAX_CONST_ARGS 8
#define CLC_MAX_READ_IMAGE_ARGS 128
#define CLC_MAX_WRITE_IMAGE_ARGS 8

#define CLC_MAX_ARGS (CLC_MAX_CONST_ARGS + CLC_MAX_READ_IMAGE_ARGS + \
                      CLC_MAX_WRITE_IMAGE_ARGS)

struct clc_dxil_metadata {
   struct {
      unsigned offset;
      unsigned size;
      unsigned buf_id;
   } *args;
   unsigned kernel_inputs_cbv_id;
   unsigned kernel_inputs_buf_size;
   unsigned global_work_offset_cbv_id;
   size_t num_uavs;

   struct {
      void *data;
      size_t size;
      unsigned cbv_id;
   } consts[CLC_MAX_CONSTS];
   size_t num_consts;

   struct {
      int image_index;
      int cbuf_offset;
   } image_channels[CLC_MAX_READ_IMAGE_ARGS + CLC_MAX_WRITE_IMAGE_ARGS];
   size_t num_image_channels;
};

struct clc_dxil_object {
   const struct clc_kernel_info *kernel;
   struct clc_dxil_metadata metadata;
   struct {
      void *data;
      size_t size;
   } binary;
};

struct clc_context {
   unsigned int dummy;
};

struct clc_context *clc_context_new(void);

void clc_free_context(struct clc_context *ctx);

struct clc_object *
clc_compile(struct clc_context *ctx,
            const struct clc_compile_args *args,
            const struct clc_logger *logger);

struct clc_object *
clc_link(struct clc_context *ctx,
         const struct clc_object **in_objs,
         unsigned num_in_objs,
         const struct clc_logger *logger);

void clc_free_object(struct clc_object *obj);

struct clc_dxil_object *
clc_to_dxil(struct clc_context *ctx,
            const struct clc_object *obj,
            const char *entrypoint,
            const struct clc_logger *logger);

void clc_free_dxil_object(struct clc_dxil_object *dxil);

#ifdef __cplusplus
}
#endif

#endif
