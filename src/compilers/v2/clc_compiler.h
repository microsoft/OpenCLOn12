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
   const struct clc_named_value *headers;
   unsigned num_headers;
   struct clc_named_value source;
   const char * const *args;
   unsigned num_args;
};

struct clc_linker_args {
   const struct clc_binary * const *in_objs;
   unsigned num_in_objs;
   unsigned create_library;
};

typedef void (*clc_msg_callback)(void *priv, const char *msg);

struct clc_logger {
   void *priv;
   clc_msg_callback error;
   clc_msg_callback warning;
};

struct clc_binary {
   void *data;
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

enum clc_vec_hint_type {
   CLC_VEC_HINT_TYPE_CHAR = 0,
   CLC_VEC_HINT_TYPE_SHORT = 1,
   CLC_VEC_HINT_TYPE_INT = 2,
   CLC_VEC_HINT_TYPE_LONG = 3,
   CLC_VEC_HINT_TYPE_HALF = 4,
   CLC_VEC_HINT_TYPE_FLOAT = 5,
   CLC_VEC_HINT_TYPE_DOUBLE = 6
};

struct clc_kernel_info {
   const char *name;
   size_t num_args;
   const struct clc_kernel_arg *args;

   unsigned vec_hint_size;
   enum clc_vec_hint_type vec_hint_type;
};

enum clc_spec_constant_type {
   CLC_SPEC_CONSTANT_UNKNOWN,
   CLC_SPEC_CONSTANT_BOOL,
   CLC_SPEC_CONSTANT_FLOAT,
   CLC_SPEC_CONSTANT_DOUBLE,
   CLC_SPEC_CONSTANT_INT8,
   CLC_SPEC_CONSTANT_UINT8,
   CLC_SPEC_CONSTANT_INT16,
   CLC_SPEC_CONSTANT_UINT16,
   CLC_SPEC_CONSTANT_INT32,
   CLC_SPEC_CONSTANT_UINT32,
   CLC_SPEC_CONSTANT_INT64,
   CLC_SPEC_CONSTANT_UINT64,
};

struct clc_parsed_spec_constant {
   uint32_t id;
   enum clc_spec_constant_type type;
};

struct clc_parsed_spirv {
   const struct clc_kernel_info *kernels;
   unsigned num_kernels;

   const struct clc_parsed_spec_constant *spec_constants;
   unsigned num_spec_constants;
};

#define CLC_MAX_CONSTS 32
#define CLC_MAX_BINDINGS_PER_ARG 3
#define CLC_MAX_SAMPLERS 16

struct clc_printf_info {
   unsigned num_args;
   unsigned *arg_sizes;
   char *str;
};

struct clc_dxil_metadata {
   struct {
      unsigned offset;
      unsigned size;
      union {
         struct {
            unsigned buf_ids[CLC_MAX_BINDINGS_PER_ARG];
            unsigned num_buf_ids;
         } image;
         struct {
            unsigned sampler_id;
         } sampler;
         struct {
            unsigned buf_id;
         } globconstptr;
         struct {
            unsigned sharedmem_offset;
	 } localptr;
      };
   } *args;
   unsigned kernel_inputs_cbv_id;
   unsigned kernel_inputs_buf_size;
   unsigned work_properties_cbv_id;
   size_t num_uavs;
   size_t num_srvs;
   size_t num_samplers;

   struct {
      void *data;
      size_t size;
      unsigned uav_id;
   } consts[CLC_MAX_CONSTS];
   size_t num_consts;

   struct {
      unsigned sampler_id;
      unsigned addressing_mode;
      unsigned normalized_coords;
      unsigned filter_mode;
   } const_samplers[CLC_MAX_SAMPLERS];
   size_t num_const_samplers;
   size_t local_mem_size;
   size_t priv_mem_size;

   uint16_t local_size[3];
   uint16_t local_size_hint[3];

   struct {
      unsigned info_count;
      struct clc_printf_info *infos;
      int uav_id;
   } printf;
};

struct clc_dxil_object {
   const struct clc_kernel_info *kernel;
   struct clc_dxil_metadata metadata;
   struct {
      void *data;
      size_t size;
   } binary;
};

struct clc_libclc {
   const void *libclc_nir;
};

struct clc_libclc_options {
   unsigned optimize;
};

struct clc_libclc *clc_libclc_new(const struct clc_logger *logger, const struct clc_libclc_options *options);

void clc_free_libclc(struct clc_libclc *lib);

void clc_libclc_serialize(struct clc_libclc *lib, void **serialized, size_t *size);
void clc_libclc_free_serialized(void *serialized);
struct clc_libclc *clc_libclc_deserialize(void *serialized, size_t size);

bool
clc_compile_c_to_spir(const struct clc_compile_args *args,
                      const struct clc_logger *logger,
                      struct clc_binary *out_spir);

void
clc_free_spir(struct clc_binary *spir);

bool
clc_compile_spir_to_spirv(const struct clc_binary *in_spir,
                          const struct clc_logger *logger,
                          struct clc_binary *out_spirv);

void
clc_free_spirv(struct clc_binary *spirv);

bool
clc_compile_c_to_spirv(const struct clc_compile_args *args,
                       const struct clc_logger *logger,
                       struct clc_binary *out_spirv);

bool
clc_link_spirv(const struct clc_linker_args *args,
               const struct clc_logger *logger,
               struct clc_binary *out_spirv);

bool
clc_parse_spirv(const struct clc_binary *in_spirv,
                const struct clc_logger *logger,
                struct clc_parsed_spirv *out_data);

void
clc_free_parsed_spirv(struct clc_parsed_spirv *data);

struct clc_runtime_arg_info {
   union {
      struct {
         unsigned size;
      } localptr;
      struct {
         unsigned normalized_coords;
         unsigned addressing_mode; /* See SPIR-V spec for value meanings */
         unsigned linear_filtering;
      } sampler;
   };
};

struct clc_runtime_kernel_conf {
   uint16_t local_size[3];
   struct clc_runtime_arg_info *args;
   unsigned lower_bit_size;
   unsigned support_global_work_id_offsets;
   unsigned support_work_group_id_offsets;
};

typedef union {
   bool b;
   float f32;
   double f64;
   int8_t i8;
   uint8_t u8;
   int16_t i16;
   uint16_t u16;
   int32_t i32;
   uint32_t u32;
   int64_t i64;
   uint64_t u64;
} clc_spirv_const_value;

struct clc_spirv_specialization {
   uint32_t id;
   clc_spirv_const_value value;
   bool defined_on_module;
};

struct clc_spirv_specialization_consts {
   const struct clc_spirv_specialization *specializations;
   unsigned num_specializations;
};

bool
clc_specialize_spirv(const struct clc_binary *in_spirv,
                     const struct clc_parsed_spirv *parsed_data,
                     const struct clc_spirv_specialization_consts *consts,
                     struct clc_binary *out_spirv);

bool
clc_spirv_to_dxil(struct clc_libclc *lib,
                  const struct clc_binary *linked_spirv,
                  const struct clc_parsed_spirv *parsed_data,
                  const char *entrypoint,
                  const struct clc_runtime_kernel_conf *conf,
                  const struct clc_spirv_specialization_consts *consts,
                  const struct clc_logger *logger,
                  struct clc_dxil_object *out_dxil);

void clc_free_dxil_object(struct clc_dxil_object *dxil);

/* This struct describes the layout of data expected in the CB bound at global_work_offset_cbv_id */
struct clc_work_properties_data {
   /* Returned from get_global_offset(), and added into get_global_id() */
   unsigned global_offset_x;
   unsigned global_offset_y;
   unsigned global_offset_z;
   /* Returned from get_work_dim() */
   unsigned work_dim;
   /* The number of work groups being launched (i.e. the parameters to Dispatch).
    * If the requested global size doesn't fit in a single Dispatch, these values should
    * indicate the total number of groups that *should* have been launched. */
   unsigned group_count_total_x;
   unsigned group_count_total_y;
   unsigned group_count_total_z;
   unsigned padding;
   /* If the requested global size doesn't fit in a single Dispatch, subsequent dispatches
    * should fill out these offsets to indicate how many groups have already been launched */
   unsigned group_id_offset_x;
   unsigned group_id_offset_y;
   unsigned group_id_offset_z;
};

uint64_t clc_compiler_get_version();

#ifdef __cplusplus
}
#endif

#endif
