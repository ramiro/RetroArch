/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <math.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../include/Cg/cg.h"
#include "../include/Cg/cgD3D9.h"

#include <retro_inline.h>
#include <retro_math.h>
#include <compat/strl.h>
#include <string/stdstring.h>

#include "../common/d3d_common.h"
#include "../drivers/d3d.h"
#include "../drivers/d3d_shaders/opaque.cg.d3d9.h"

#include "../video_driver.h"
#include "../../configuration.h"
#include "../../verbosity.h"

#define D3D_DEFAULT_NONPOW2         ((UINT)-2)
#define D3D_FILTER_LINEAR           (3 << 0)
#define D3D_FILTER_POINT            (2 << 0)

#define d3d9_cg_set_param_1f(param, x) if (param) cgD3D9SetUniform(param, x)

#define D3D_PI 3.14159265358979323846264338327

#define set_cg_param(prog, param, val) do { \
   CGparameter cgp = cgGetNamedParameter(prog, param); \
   if (cgp) \
      cgD3D9SetUniform(cgp, &val); \
} while(0)

#define VECTOR_LIST_TYPE unsigned
#define VECTOR_LIST_NAME unsigned
#include "../../libretro-common/lists/vector_list.c"
#undef VECTOR_LIST_TYPE
#undef VECTOR_LIST_NAME

struct lut_info
{
   LPDIRECT3DTEXTURE9 tex;
   char id[64];
   bool smooth;
};

struct CGVertex
{
   float x, y, z;
   float u, v;
   float lut_u, lut_v;
   float r, g, b, a;
};

struct Pass
{
   unsigned last_width, last_height;
   struct LinkInfo info;
   D3DPOOL pool;
   LPDIRECT3DTEXTURE9 tex;
   LPDIRECT3DVERTEXBUFFER9 vertex_buf;
   CGprogram vPrg, fPrg;
   LPDIRECT3DVERTEXDECLARATION9 vertex_decl;
   struct unsigned_vector_list *attrib_map;
};

#define VECTOR_LIST_TYPE struct Pass
#define VECTOR_LIST_NAME pass
#include "../../libretro-common/lists/vector_list.c"
#undef VECTOR_LIST_TYPE
#undef VECTOR_LIST_NAME

#define VECTOR_LIST_TYPE struct lut_info
#define VECTOR_LIST_NAME lut_info
#include "../../libretro-common/lists/vector_list.c"
#undef VECTOR_LIST_TYPE
#undef VECTOR_LIST_NAME

typedef struct cg_renderchain
{
   unsigned pixel_size;
   unsigned frame_count;
   struct
   {
      LPDIRECT3DTEXTURE9 tex[TEXTURES];
      LPDIRECT3DVERTEXBUFFER9 vertex_buf[TEXTURES];
      unsigned ptr;
      unsigned last_width[TEXTURES];
      unsigned last_height[TEXTURES];
   } prev;
   CGprogram vStock;
   CGprogram fStock;
   void *dev;
   const video_info_t *video_info;
   D3DVIEWPORT9 *final_viewport;
   struct pass_vector_list *passes;
   struct unsigned_vector_list *bound_tex;
   struct unsigned_vector_list *bound_vert;
   struct lut_info_vector_list *luts;
   state_tracker_t *state_tracker;
   CGcontext cgCtx;
} cg_renderchain_t;

static INLINE bool d3d9_cg_validate_param_name(const char *name)
{
   unsigned i;
   static const char *illegal[] = {
      "PREV.",
      "PREV1.",
      "PREV2.",
      "PREV3.",
      "PREV4.",
      "PREV5.",
      "PREV6.",
      "ORIG.",
      "IN.",
      "PASS",
   };

   if (!name)
      return false;

   for (i = 0; i < sizeof(illegal) / sizeof(illegal[0]); i++)
      if (strstr(name, illegal[i]) == name)
         return false;

   return true;
}

static INLINE CGparameter d3d9_cg_find_param_from_semantic(
      CGparameter param, const char *sem)
{
   for (; param; param = cgGetNextParameter(param))
   {
      const char *semantic = NULL;
      if (cgGetParameterType(param) == CG_STRUCT)
      {
         CGparameter ret = d3d9_cg_find_param_from_semantic(
               cgGetFirstStructParameter(param), sem);

         if (ret)
            return ret;
      }

      if (     cgGetParameterDirection(param) != CG_IN
            || cgGetParameterVariability(param) != CG_VARYING)
         continue;

      semantic = cgGetParameterSemantic(param);
      if (!semantic)
         continue;

      if (string_is_equal(sem, semantic) &&
            d3d9_cg_validate_param_name(cgGetParameterName(param)))
         return param;
   }

   return NULL;
}

static bool d3d9_cg_load_program(void *data,
      void *fragment_data, void *vertex_data,
      const char *prog, bool path_is_file)
{
   const char *list           = NULL;
   char *listing_f            = NULL;
   char *listing_v            = NULL;
   CGprogram *fPrg            = (CGprogram*)fragment_data;
   CGprogram *vPrg            = (CGprogram*)vertex_data;
   CGprofile vertex_profile   = cgD3D9GetLatestVertexProfile();
   CGprofile fragment_profile = cgD3D9GetLatestPixelProfile();
   const char **fragment_opts = cgD3D9GetOptimalOptions(fragment_profile);
   const char **vertex_opts   = cgD3D9GetOptimalOptions(vertex_profile);
   cg_renderchain_t *cg_data  = (cg_renderchain_t*)data;

   if (
         fragment_profile == CG_PROFILE_UNKNOWN ||
         vertex_profile   == CG_PROFILE_UNKNOWN)
   {
      RARCH_ERR("Invalid profile type\n");
      goto error;
   }

   RARCH_LOG("[D3D Cg]: Vertex profile: %s\n", cgGetProfileString(vertex_profile));
   RARCH_LOG("[D3D Cg]: Fragment profile: %s\n", cgGetProfileString(fragment_profile));

   if (path_is_file && !string_is_empty(prog))
      *fPrg = cgCreateProgramFromFile(cg_data->cgCtx, CG_SOURCE,
            prog, fragment_profile, "main_fragment", fragment_opts);
   else
      *fPrg = cgCreateProgram(cg_data->cgCtx, CG_SOURCE, stock_cg_d3d9_program,
            fragment_profile, "main_fragment", fragment_opts);

   list = cgGetLastListing(cg_data->cgCtx);
   if (list)
      listing_f = strdup(list);

   if (path_is_file && !string_is_empty(prog))
      *vPrg = cgCreateProgramFromFile(cg_data->cgCtx, CG_SOURCE,
            prog, vertex_profile, "main_vertex", vertex_opts);
   else
      *vPrg = cgCreateProgram(cg_data->cgCtx, CG_SOURCE, stock_cg_d3d9_program,
            vertex_profile, "main_vertex", vertex_opts);

   list = cgGetLastListing(cg_data->cgCtx);
   if (list)
      listing_v = strdup(list);

   if (!fPrg || !vPrg)
      goto error;

   cgD3D9LoadProgram(*fPrg, true, 0);
   cgD3D9LoadProgram(*vPrg, true, 0);

   free(listing_f);
   free(listing_v);

   return true;

error:
   RARCH_ERR("CG error: %s\n", cgGetErrorString(cgGetError()));
   if (listing_f)
      RARCH_ERR("Fragment:\n%s\n", listing_f);
   else if (listing_v)
      RARCH_ERR("Vertex:\n%s\n", listing_v);
   free(listing_f);
   free(listing_v);

   return false;
}

static void d3d9_cg_renderchain_set_shader_params(
      cg_renderchain_t *chain,
      struct Pass *pass,
      unsigned video_w, unsigned video_h,
      unsigned tex_w, unsigned tex_h,
      unsigned viewport_w, unsigned viewport_h)
{
   float frame_cnt;
   float video_size[2];
   float texture_size[2];
   float output_size[2];

   video_size[0]        = video_w;
   video_size[1]        = video_h;
   texture_size[0]      = tex_w;
   texture_size[1]      = tex_h;
   output_size[0]       = viewport_w;
   output_size[1]       = viewport_h;

   set_cg_param(pass->vPrg, "IN.video_size", video_size);
   set_cg_param(pass->fPrg, "IN.video_size", video_size);
   set_cg_param(pass->vPrg, "IN.texture_size", texture_size);
   set_cg_param(pass->fPrg, "IN.texture_size", texture_size);
   set_cg_param(pass->vPrg, "IN.output_size", output_size);
   set_cg_param(pass->fPrg, "IN.output_size", output_size);

   frame_cnt            = chain->frame_count;

   if (pass->info.pass->frame_count_mod)
      frame_cnt         = chain->frame_count % pass->info.pass->frame_count_mod;

   set_cg_param(pass->fPrg, "IN.frame_count", frame_cnt);
   set_cg_param(pass->vPrg, "IN.frame_count", frame_cnt);
}

#define DECL_FVF_TEXCOORD(stream, offset, index) \
   { (WORD)(stream), (WORD)(offset * sizeof(float)), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, \
      D3DDECLUSAGE_TEXCOORD, (BYTE)(index) }
#define DECL_FVF_COLOR(stream, offset, index) \
   { (WORD)(stream), (WORD)(offset * sizeof(float)), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, \
      D3DDECLUSAGE_COLOR, (BYTE)(index) } \

static bool d3d9_cg_renderchain_init_shader_fvf(void *data, void *pass_data)
{
   CGparameter param;
   unsigned index, i, count;
   unsigned tex_index                          = 0;
   bool texcoord0_taken                        = false;
   bool texcoord1_taken                        = false;
   bool stream_taken[4]                        = {false};
   cg_renderchain_t *chain                     = (cg_renderchain_t*)data;
   struct Pass          *pass                  = (struct Pass*)pass_data;
   static const D3DVERTEXELEMENT9 decl_end     = D3DDECL_END();
   D3DVERTEXELEMENT9 decl[MAXD3DDECLLENGTH]    = {{0}};
   bool *indices                               = NULL;

   if (cgD3D9GetVertexDeclaration(pass->vPrg, decl) == CG_FALSE)
      return false;

   for (count = 0; count < MAXD3DDECLLENGTH; count++)
   {
      if (string_is_equal_fast(&decl_end, &decl[count], sizeof(decl_end)))
         break;
   }

   /* This is completely insane.
    * We do not have a good and easy way of setting up our
    * attribute streams, so we have to do it ourselves, yay!
    *
    * Stream 0      => POSITION
    * Stream 1      => TEXCOORD0
    * Stream 2      => TEXCOORD1
    * Stream 3      => COLOR     (Not really used for anything.)
    * Stream {4..N} => Texture coord streams for varying resources
    *                  which have no semantics.
    */

   indices = (bool*)calloc(1, count * sizeof(*indices));

   param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "POSITION");
   if (!param)
      param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "POSITION0");

   if (param)
   {
      static const D3DVERTEXELEMENT9 element =
      {
         0, 0 * sizeof(float),
         D3DDECLTYPE_FLOAT3,
         D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION,
         0
      };
      stream_taken[0] = true;
      index           = cgGetParameterResourceIndex(param);
      decl[index]     = element;
      indices[index]  = true;

      RARCH_LOG("[FVF]: POSITION semantic found.\n");
   }

   param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "TEXCOORD");
   if (!param)
      param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "TEXCOORD0");

   if (param)
   {
      static const D3DVERTEXELEMENT9 tex_coord0    = DECL_FVF_TEXCOORD(1, 3, 0);
      stream_taken[1] = true;
      texcoord0_taken = true;
      RARCH_LOG("[FVF]: TEXCOORD0 semantic found.\n");
      index           = cgGetParameterResourceIndex(param);
      decl[index]     = tex_coord0;
      indices[index]  = true;
   }

   param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "TEXCOORD1");
   if (param)
   {
      static const D3DVERTEXELEMENT9 tex_coord1    = DECL_FVF_TEXCOORD(2, 5, 1);
      stream_taken[2] = true;
      texcoord1_taken = true;
      RARCH_LOG("[FVF]: TEXCOORD1 semantic found.\n");
      index           = cgGetParameterResourceIndex(param);
      decl[index]     = tex_coord1;
      indices[index]  = true;
   }

   param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "COLOR");
   if (!param)
      param = d3d9_cg_find_param_from_semantic(cgGetFirstParameter(pass->vPrg, CG_PROGRAM), "COLOR0");

   if (param)
   {
      static const D3DVERTEXELEMENT9 color = DECL_FVF_COLOR(3, 7, 0);
      stream_taken[3] = true;
      RARCH_LOG("[FVF]: COLOR0 semantic found.\n");
      index           = cgGetParameterResourceIndex(param);
      decl[index]     = color;
      indices[index]  = true;
   }

   /* Stream {0, 1, 2, 3} might be already taken. Find first vacant stream. */
   for (index = 0; index < 4; index++)
   {
      if (stream_taken[index] == false)
         break;
   }

   /* Find first vacant texcoord declaration. */
   if (texcoord0_taken && texcoord1_taken)
      tex_index = 2;
   else if (texcoord1_taken && !texcoord0_taken)
      tex_index = 0;
   else if (texcoord0_taken && !texcoord1_taken)
      tex_index = 1;

   for (i = 0; i < count; i++)
   {
      if (indices[i])
         unsigned_vector_list_append(pass->attrib_map, 0);
      else
      {
         D3DVERTEXELEMENT9 elem = DECL_FVF_TEXCOORD(index, 3, tex_index);

         unsigned_vector_list_append(pass->attrib_map, index);

         decl[i]     = elem;

         /* Find next vacant stream. */
         while ((++index < 4) && stream_taken[index])
            index++;

         /* Find next vacant texcoord declaration. */
         if ((++tex_index == 1) && texcoord1_taken)
            tex_index++;
      }
   }

   free(indices);

   return d3d_vertex_declaration_new(chain->dev,
         decl, (void**)&pass->vertex_decl);
}

static void d3d9_cg_renderchain_bind_orig(cg_renderchain_t *chain,
      void *pass_data)
{
   unsigned index;
   CGparameter param;
   float video_size[2];
   float texture_size[2];
   struct Pass    *pass = (struct Pass*)pass_data;
   video_size[0]        = chain->passes->data[0].last_width;
   video_size[1]        = chain->passes->data[0].last_height;
   texture_size[0]      = chain->passes->data[0].info.tex_w;
   texture_size[1]      = chain->passes->data[0].info.tex_h;

   set_cg_param(pass->vPrg, "ORIG.video_size", video_size);
   set_cg_param(pass->fPrg, "ORIG.video_size", video_size);
   set_cg_param(pass->vPrg, "ORIG.texture_size", texture_size);
   set_cg_param(pass->fPrg, "ORIG.texture_size", texture_size);

   param = cgGetNamedParameter(pass->fPrg, "ORIG.texture");
   if (param)
   {
      index = cgGetParameterResourceIndex(param);
      d3d_set_texture(chain->dev, index, chain->passes->data[0].tex);
      d3d_set_sampler_magfilter(chain->dev, index,
            d3d_translate_filter(chain->passes->data[0].info.pass->filter));
      d3d_set_sampler_minfilter(chain->dev, index,
            d3d_translate_filter(chain->passes->data[0].info.pass->filter));
      d3d_set_sampler_address_u(chain->dev, index, D3DTADDRESS_BORDER);
      d3d_set_sampler_address_v(chain->dev, index, D3DTADDRESS_BORDER);
      unsigned_vector_list_append(chain->bound_tex, index);
   }

   param = cgGetNamedParameter(pass->vPrg, "ORIG.tex_coord");
   if (param)
   {
      LPDIRECT3DVERTEXBUFFER9 vert_buf = (LPDIRECT3DVERTEXBUFFER9)chain->passes->data[0].vertex_buf;

      index = pass->attrib_map->data[cgGetParameterResourceIndex(param)];

      d3d_set_stream_source(chain->dev, index,
            vert_buf, 0, sizeof(struct CGVertex));
      unsigned_vector_list_append(chain->bound_vert, index);
   }
}

static void d3d9_cg_renderchain_bind_prev(void *data, const void *pass_data)
{
   unsigned i, index;
   float texture_size[2];
   char attr_texture[64]    = {0};
   char attr_input_size[64] = {0};
   char attr_tex_size[64]   = {0};
   char attr_coord[64]      = {0};
   cg_renderchain_t *chain  = (cg_renderchain_t*)data;
   struct Pass       *pass  = (struct Pass*)pass_data;
   static const char *prev_names[] = {
      "PREV",
      "PREV1",
      "PREV2",
      "PREV3",
      "PREV4",
      "PREV5",
      "PREV6",
   };

   texture_size[0] = chain->passes->data[0].info.tex_w;
   texture_size[1] = chain->passes->data[0].info.tex_h;

   for (i = 0; i < TEXTURES - 1; i++)
   {
      CGparameter param;
      float video_size[2];

      snprintf(attr_texture,    sizeof(attr_texture),    "%s.texture",      prev_names[i]);
      snprintf(attr_input_size, sizeof(attr_input_size), "%s.video_size",   prev_names[i]);
      snprintf(attr_tex_size,   sizeof(attr_tex_size),   "%s.texture_size", prev_names[i]);
      snprintf(attr_coord,      sizeof(attr_coord),      "%s.tex_coord",    prev_names[i]);

      video_size[0] = chain->prev.last_width[(chain->prev.ptr - (i + 1)) & TEXTURESMASK];
      video_size[1] = chain->prev.last_height[(chain->prev.ptr - (i + 1)) & TEXTURESMASK];

      set_cg_param(pass->vPrg, attr_input_size, video_size);
      set_cg_param(pass->fPrg, attr_input_size, video_size);
      set_cg_param(pass->vPrg, attr_tex_size, texture_size);
      set_cg_param(pass->fPrg, attr_tex_size, texture_size);

      param = cgGetNamedParameter(pass->fPrg, attr_texture);
      if (param)
      {
         LPDIRECT3DTEXTURE9 tex;

         index = cgGetParameterResourceIndex(param);

         tex = (LPDIRECT3DTEXTURE9)
            chain->prev.tex[(chain->prev.ptr - (i + 1)) & TEXTURESMASK];

         d3d_set_texture(chain->dev, index, tex);
         unsigned_vector_list_append(chain->bound_tex, index);

         d3d_set_sampler_magfilter(chain->dev, index,
               d3d_translate_filter(chain->passes->data[0].info.pass->filter));
         d3d_set_sampler_minfilter(chain->dev, index,
               d3d_translate_filter(chain->passes->data[0].info.pass->filter));
         d3d_set_sampler_address_u(chain->dev, index, D3DTADDRESS_BORDER);
         d3d_set_sampler_address_v(chain->dev, index, D3DTADDRESS_BORDER);
      }

      param = cgGetNamedParameter(pass->vPrg, attr_coord);
      if (param)
      {
         LPDIRECT3DVERTEXBUFFER9 vert_buf = (LPDIRECT3DVERTEXBUFFER9)
            chain->prev.vertex_buf[(chain->prev.ptr - (i + 1)) & TEXTURESMASK];

         index = pass->attrib_map->data[cgGetParameterResourceIndex(param)];

         d3d_set_stream_source(chain->dev, index,
               vert_buf, 0, sizeof(struct CGVertex));
         unsigned_vector_list_append(chain->bound_vert, index);
      }
   }
}

static void d3d9_cg_renderchain_add_lut_internal(void *data,
      unsigned index, unsigned i)
{
   cg_renderchain_t *chain = (cg_renderchain_t*)data;
   if (!chain)
      return;

   d3d_set_texture(chain->dev, index, chain->luts->data[i].tex);
   d3d_set_sampler_magfilter(chain->dev, index,
         d3d_translate_filter(chain->luts->data[i].smooth ? RARCH_FILTER_LINEAR : RARCH_FILTER_NEAREST));
   d3d_set_sampler_minfilter(chain->dev, index,
         d3d_translate_filter(chain->luts->data[i].smooth ? RARCH_FILTER_LINEAR : RARCH_FILTER_NEAREST));
   d3d_set_sampler_address_u(chain->dev, index, D3DTADDRESS_BORDER);
   d3d_set_sampler_address_v(chain->dev, index, D3DTADDRESS_BORDER);
   unsigned_vector_list_append(chain->bound_tex, index);
}

static void d3d9_cg_renderchain_bind_pass(
      cg_renderchain_t *chain,
      struct Pass *pass, unsigned pass_index)
{
   unsigned i, index;

   /* We only bother binding passes which are two indices behind. */
   if (pass_index < 3)
      return;

   for (i = 1; i < pass_index - 1; i++)
   {
      CGparameter param;
      float video_size[2];
      float texture_size[2];
      char pass_base[64]       = {0};
      char attr_texture[64]    = {0};
      char attr_input_size[64] = {0};
      char attr_tex_size[64]   = {0};
      char attr_coord[64]      = {0};

      snprintf(pass_base,       sizeof(pass_base),       "PASS%u",          i);
      snprintf(attr_texture,    sizeof(attr_texture),    "%s.texture",      pass_base);
      snprintf(attr_input_size, sizeof(attr_input_size), "%s.video_size",   pass_base);
      snprintf(attr_tex_size,   sizeof(attr_tex_size),   "%s.texture_size", pass_base);
      snprintf(attr_coord,      sizeof(attr_coord),      "%s.tex_coord",    pass_base);

      video_size[0]  = chain->passes->data[i].last_width;
      video_size[1]  = chain->passes->data[i].last_height;
      texture_size[0] = chain->passes->data[i].info.tex_w;
      texture_size[1] = chain->passes->data[i].info.tex_h;

      set_cg_param(pass->vPrg, attr_input_size,   video_size);
      set_cg_param(pass->fPrg, attr_input_size,   video_size);
      set_cg_param(pass->vPrg, attr_tex_size,     texture_size);
      set_cg_param(pass->fPrg, attr_tex_size,     texture_size);

      param = cgGetNamedParameter(pass->fPrg, attr_texture);
      if (param)
      {
         index = cgGetParameterResourceIndex(param);
         unsigned_vector_list_append(chain->bound_tex, index);

         d3d_set_texture(chain->dev, index, chain->passes->data[i].tex);
         d3d_set_sampler_magfilter(chain->dev, index,
               d3d_translate_filter(chain->passes->data[i].info.pass->filter));
         d3d_set_sampler_minfilter(chain->dev, index,
               d3d_translate_filter(chain->passes->data[i].info.pass->filter));
         d3d_set_sampler_address_u(chain->dev, index, D3DTADDRESS_BORDER);
         d3d_set_sampler_address_v(chain->dev, index, D3DTADDRESS_BORDER);
      }

      param = cgGetNamedParameter(pass->vPrg, attr_coord);
      if (param)
      {
         index = pass->attrib_map->data[cgGetParameterResourceIndex(param)];

         d3d_set_stream_source(chain->dev, index, chain->passes->data[i].vertex_buf,
               0, sizeof(struct CGVertex));
         unsigned_vector_list_append(chain->bound_vert, index);
      }
   }
}

static void d3d9_cg_deinit_progs(void *data)
{
   unsigned i;
   cg_renderchain_t *cg_data = (cg_renderchain_t*)data;

   if (!cg_data)
      return;

   RARCH_LOG("CG: Destroying programs.\n");

   if (cg_data->passes->count >= 1)
   {
      d3d_vertex_buffer_free(NULL, cg_data->passes->data[0].vertex_decl);

      for (i = 1; i < cg_data->passes->count; i++)
      {
         if (cg_data->passes->data[i].tex)
            d3d_texture_free(cg_data->passes->data[i].tex);
         cg_data->passes->data[i].tex = NULL;
         d3d_vertex_buffer_free(
               cg_data->passes->data[i].vertex_buf,
               cg_data->passes->data[i].vertex_decl);

         if (cg_data->passes->data[i].fPrg)
            cgDestroyProgram(cg_data->passes->data[i].fPrg);
         if (cg_data->passes->data[i].vPrg)
            cgDestroyProgram(cg_data->passes->data[i].vPrg);
      }
   }

   if (cg_data->fStock)
      cgDestroyProgram(cg_data->fStock);
   if (cg_data->vStock)
      cgDestroyProgram(cg_data->vStock);
}

static void d3d9_cg_destroy_resources(void *data)
{
   unsigned i;
   cg_renderchain_t *cg_data = (cg_renderchain_t*)data;

   for (i = 0; i < TEXTURES; i++)
   {
      if (cg_data->prev.tex[i])
         d3d_texture_free(cg_data->prev.tex[i]);
      if (cg_data->prev.vertex_buf[i])
         d3d_vertex_buffer_free(cg_data->prev.vertex_buf[i], NULL);
   }

   d3d9_cg_deinit_progs(cg_data);

   for (i = 0; i < cg_data->luts->count; i++)
   {
      if (cg_data->luts->data[i].tex)
         d3d_texture_free(cg_data->luts->data[i].tex);
   }

   if (cg_data->state_tracker)
   {
      state_tracker_free(cg_data->state_tracker);
      cg_data->state_tracker = NULL;
   }

   cgD3D9UnloadAllPrograms();
   cgD3D9SetDevice(NULL);
}

static void d3d9_cg_deinit_context_state(void *data)
{
   cg_renderchain_t *cg_data = (cg_renderchain_t*)data;
   if (cg_data->cgCtx)
   {
      RARCH_LOG("CG: Destroying context.\n");
      cgDestroyContext(cg_data->cgCtx);
   }
   cg_data->cgCtx = NULL;
}

void d3d9_cg_renderchain_free(void *data)
{
   cg_renderchain_t *cg_data = (cg_renderchain_t*)data;

   if (!cg_data)
      return;

   d3d9_cg_destroy_resources(cg_data);

   if (cg_data->passes)
   {
      unsigned i;

      for (i = 0; i < cg_data->passes->count; i++)
      {
         if (cg_data->passes->data[i].attrib_map)
            free(cg_data->passes->data[i].attrib_map);
      }

      pass_vector_list_free(cg_data->passes);

      cg_data->passes = NULL;
   }

   lut_info_vector_list_free(cg_data->luts);
   unsigned_vector_list_free(cg_data->bound_tex);
   unsigned_vector_list_free(cg_data->bound_vert);

   cg_data->luts = NULL;
   cg_data->bound_tex = NULL;
   cg_data->bound_vert = NULL;

   d3d9_cg_deinit_context_state(cg_data);

   free(cg_data);
}

static void *d3d9_cg_renderchain_new(void)
{
   cg_renderchain_t *renderchain = (cg_renderchain_t*)calloc(1, sizeof(*renderchain));
   if (!renderchain)
      return NULL;

   renderchain->passes     = pass_vector_list_new();
   renderchain->luts       = lut_info_vector_list_new();
   renderchain->bound_tex  = unsigned_vector_list_new();
   renderchain->bound_vert = unsigned_vector_list_new();

   return renderchain;
}

static bool d3d9_cg_renderchain_init_shader(void *data,
      void *renderchain_data)
{
   d3d_video_t *d3d              = (d3d_video_t*)data;
   cg_renderchain_t *renderchain = (cg_renderchain_t*)renderchain_data;

   if (!d3d || !renderchain)
      return false;

   renderchain->cgCtx = cgCreateContext();
   if (!renderchain->cgCtx)
   {
      RARCH_ERR("Failed to create Cg context.\n");
      return false;
   }

   if (FAILED(cgD3D9SetDevice((IDirect3DDevice9*)d3d->dev)))
      return false;
   return true;
}

static void d3d9_cg_renderchain_log_info(
      void *data, const void *info_data)
{
   const struct LinkInfo *info = (const struct LinkInfo*)info_data;
   RARCH_LOG("[D3D]: Render pass info:\n");
   RARCH_LOG("\tTexture width: %u\n", info->tex_w);
   RARCH_LOG("\tTexture height: %u\n", info->tex_h);

   RARCH_LOG("\tScale type (X): ");

   switch (info->pass->fbo.type_x)
   {
      case RARCH_SCALE_INPUT:
         RARCH_LOG("Relative @ %fx\n", info->pass->fbo.scale_x);
         break;

      case RARCH_SCALE_VIEWPORT:
         RARCH_LOG("Viewport @ %fx\n", info->pass->fbo.scale_x);
         break;

      case RARCH_SCALE_ABSOLUTE:
         RARCH_LOG("Absolute @ %u px\n", info->pass->fbo.abs_x);
         break;
   }

   RARCH_LOG("\tScale type (Y): ");

   switch (info->pass->fbo.type_y)
   {
      case RARCH_SCALE_INPUT:
         RARCH_LOG("Relative @ %fx\n", info->pass->fbo.scale_y);
         break;

      case RARCH_SCALE_VIEWPORT:
         RARCH_LOG("Viewport @ %fx\n", info->pass->fbo.scale_y);
         break;

      case RARCH_SCALE_ABSOLUTE:
         RARCH_LOG("Absolute @ %u px\n", info->pass->fbo.abs_y);
         break;
   }

   RARCH_LOG("\tBilinear filter: %s\n",
         info->pass->filter == RARCH_FILTER_LINEAR ? "true" : "false");
}

static bool d3d9_cg_renderchain_create_first_pass(
      cg_renderchain_t *chain,
      const struct LinkInfo *info, unsigned fmt)
{
   unsigned i;
   struct Pass pass;
   D3DMATRIX ident;

   if (!chain)
      return false;

   pass.attrib_map = unsigned_vector_list_new();

   d3d_matrix_identity(&ident);

   d3d_set_transform(chain->dev, D3DTS_WORLD, &ident);
   d3d_set_transform(chain->dev, D3DTS_VIEW, &ident);

   pass.info        = *info;
   pass.last_width  = 0;
   pass.last_height = 0;
   pass.attrib_map  = unsigned_vector_list_new();

   chain->prev.ptr  = 0;

   for (i = 0; i < TEXTURES; i++)
   {
      chain->prev.last_width[i]  = 0;
      chain->prev.last_height[i] = 0;
      chain->prev.vertex_buf[i]  = (LPDIRECT3DVERTEXBUFFER9)
         d3d_vertex_buffer_new(
            chain->dev, 4 * sizeof(struct CGVertex),
            D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, NULL);

      if (!chain->prev.vertex_buf[i])
         return false;

      chain->prev.tex[i] = (LPDIRECT3DTEXTURE9)
         d3d_texture_new(chain->dev, NULL,
            info->tex_w, info->tex_h, 1, 0,
            (fmt == RETRO_PIXEL_FORMAT_RGB565) ? 
            d3d_get_rgb565_format() : d3d_get_xrgb8888_format(),
            D3DPOOL_MANAGED, 0, 0, 0, NULL, NULL, false);

      if (!chain->prev.tex[i])
         return false;

      d3d_set_texture(chain->dev, 0, chain->prev.tex[i]);
      d3d_set_sampler_minfilter(chain->dev, 0,
            d3d_translate_filter(info->pass->filter));
      d3d_set_sampler_magfilter(chain->dev, 0,
            d3d_translate_filter(info->pass->filter));
      d3d_set_sampler_address_u(chain->dev, 0, D3DTADDRESS_BORDER);
      d3d_set_sampler_address_v(chain->dev, 0, D3DTADDRESS_BORDER);
      d3d_set_texture(chain->dev, 0, NULL);
   }

   d3d9_cg_load_program(chain, &pass.fPrg,
         &pass.vPrg, info->pass->source.path, true);

   if (!d3d9_cg_renderchain_init_shader_fvf(chain, &pass))
      return false;
   pass_vector_list_append(chain->passes, pass);
   return true;
}

static bool d3d9_cg_renderchain_init(void *data,
      const void *_video_info,
      void *dev_,
      const void *final_viewport_,
      const void *info_data, bool rgb32)
{
   const struct LinkInfo *info    = (const struct LinkInfo*)info_data;
   d3d_video_t *d3d               = (d3d_video_t*)data;
   cg_renderchain_t *chain        = (cg_renderchain_t*)d3d->renderchain_data;
   const video_info_t *video_info = (const video_info_t*)_video_info;
   unsigned fmt                   = (rgb32) ? RETRO_PIXEL_FORMAT_XRGB8888 : RETRO_PIXEL_FORMAT_RGB565;

   if (!chain)
      return false;
   if (!d3d9_cg_renderchain_init_shader(d3d, chain))
   {
      RARCH_ERR("Failed to initialize shader subsystem.\n");
      return false;
   }

   chain->dev            = (void*)dev_;
   chain->video_info     = video_info;
   chain->state_tracker  = NULL;
   chain->final_viewport = (D3DVIEWPORT9*)final_viewport_;
   chain->frame_count    = 0;
   chain->pixel_size     = (fmt == RETRO_PIXEL_FORMAT_RGB565) ? 2 : 4;

   if (!d3d9_cg_renderchain_create_first_pass(chain, info, fmt))
      return false;
   d3d9_cg_renderchain_log_info(chain, info);
   if (!d3d9_cg_load_program(chain, &chain->fStock, &chain->vStock, NULL, false))
      return false;

   cgD3D9BindProgram(chain->fStock);
   cgD3D9BindProgram(chain->vStock);

   return true;
}

static bool d3d9_cg_renderchain_set_pass_size(
      cg_renderchain_t *chain,
      unsigned pass_index, unsigned width, unsigned height)
{
   struct Pass *pass = (struct Pass*)&chain->passes->data[pass_index];

   if (width != pass->info.tex_w || height != pass->info.tex_h)
   {
      d3d_texture_free(pass->tex);

      pass->info.tex_w = width;
      pass->info.tex_h = height;
      pass->pool       = D3DPOOL_DEFAULT;
      pass->tex        = (LPDIRECT3DTEXTURE9)
         d3d_texture_new(chain->dev, NULL,
            width, height, 1,
            D3DUSAGE_RENDERTARGET,
            chain->passes->data[chain->passes->count - 1].info.pass->fbo.fp_fbo ?
            D3DFMT_A32B32G32R32F : d3d_get_argb8888_format(),
            D3DPOOL_DEFAULT, 0, 0, 0,
            NULL, NULL, false);

      if (!pass->tex)
         return false;

      d3d_set_texture(chain->dev, 0, pass->tex);
      d3d_set_sampler_address_u(chain->dev, 0, D3DTADDRESS_BORDER);
      d3d_set_sampler_address_v(chain->dev, 0, D3DTADDRESS_BORDER);
      d3d_set_texture(chain->dev, 0, NULL);
   }

   return true;
}

static void d3d9_cg_renderchain_convert_geometry(
      void *data,
      const void *info_data,
      unsigned *out_width,
      unsigned *out_height,
      unsigned width,
      unsigned height,
      void *final_viewport_data)
{
   const struct LinkInfo *info                 = (const struct LinkInfo*)info_data;
   cg_renderchain_t *chain                     = (cg_renderchain_t*)data;
   D3DVIEWPORT9 *final_viewport                = (D3DVIEWPORT9*)final_viewport_data;

   if (!chain || !info)
      return;

   switch (info->pass->fbo.type_x)
   {
      case RARCH_SCALE_VIEWPORT:
         *out_width = info->pass->fbo.scale_x * final_viewport->Width;
         break;

      case RARCH_SCALE_ABSOLUTE:
         *out_width = info->pass->fbo.abs_x;
         break;

      case RARCH_SCALE_INPUT:
         *out_width = info->pass->fbo.scale_x * width;
         break;
   }

   switch (info->pass->fbo.type_y)
   {
      case RARCH_SCALE_VIEWPORT:
         *out_height = info->pass->fbo.scale_y * final_viewport->Height;
         break;

      case RARCH_SCALE_ABSOLUTE:
         *out_height = info->pass->fbo.abs_y;
         break;

      case RARCH_SCALE_INPUT:
         *out_height = info->pass->fbo.scale_y * height;
         break;
   }
}

static void d3d_recompute_pass_sizes(cg_renderchain_t *chain,
      d3d_video_t *d3d)
{
   unsigned i;
   struct LinkInfo link_info;
   unsigned current_width            = d3d->video_info.input_scale * RARCH_SCALE_BASE;
   unsigned current_height           = d3d->video_info.input_scale * RARCH_SCALE_BASE;
   unsigned out_width                = 0;
   unsigned out_height               = 0;

   link_info.pass                    = &d3d->shader.pass[0];
   link_info.tex_w                   = current_width;
   link_info.tex_h                   = current_height;

   if (!d3d9_cg_renderchain_set_pass_size(chain, 0,
            current_width, current_height))
   {
      RARCH_ERR("[D3D]: Failed to set pass size.\n");
      return;
   }

   for (i = 1; i < d3d->shader.passes; i++)
   {
      d3d9_cg_renderchain_convert_geometry(chain,
            &link_info,
            &out_width, &out_height,
            current_width, current_height, &d3d->final_viewport);

      link_info.tex_w = next_pow2(out_width);
      link_info.tex_h = next_pow2(out_height);

      if (!d3d9_cg_renderchain_set_pass_size(chain, i,
               link_info.tex_w, link_info.tex_h))
      {
         RARCH_ERR("[D3D]: Failed to set pass size.\n");
         return;
      }

      current_width  = out_width;
      current_height = out_height;

      link_info.pass = &d3d->shader.pass[i];
   }
}

static void d3d9_cg_renderchain_set_final_viewport(
      void *data,
      void *renderchain_data,
      const void *viewport_data)
{
   d3d_video_t                  *d3d    = (d3d_video_t*)data;
   cg_renderchain_t              *chain = (cg_renderchain_t*)renderchain_data;
   const D3DVIEWPORT9 *final_viewport   = (const D3DVIEWPORT9*)viewport_data;

   if (chain && final_viewport)
      chain->final_viewport = (D3DVIEWPORT9*)final_viewport;

   d3d_recompute_pass_sizes(chain, d3d);
}

static bool d3d9_cg_renderchain_add_pass(
      void *data,
      const void *info_data)
{
   struct Pass pass;
   const struct LinkInfo *info = (const struct LinkInfo*)info_data;
   cg_renderchain_t *chain     = (cg_renderchain_t*)data;

   pass.info                   = *info;
   pass.last_width             = 0;
   pass.last_height            = 0;
   pass.attrib_map             = unsigned_vector_list_new();
   pass.pool                   = D3DPOOL_DEFAULT;

   d3d9_cg_load_program(chain, &pass.fPrg,
         &pass.vPrg, info->pass->source.path, true);

   if (!d3d9_cg_renderchain_init_shader_fvf(chain, &pass))
      return false;

   pass.vertex_buf = (LPDIRECT3DVERTEXBUFFER9)
      d3d_vertex_buffer_new(chain->dev,
         4 * sizeof(struct CGVertex),
         D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, NULL);

   if (!pass.vertex_buf)
      return false;

   pass.tex = (LPDIRECT3DTEXTURE9)d3d_texture_new(
         chain->dev,
         NULL,
         info->tex_w,
         info->tex_h,
         1,
         D3DUSAGE_RENDERTARGET,
         chain->passes->data[chain->passes->count - 1].info.pass->fbo.fp_fbo
         ? D3DFMT_A32B32G32R32F : d3d_get_argb8888_format(),
         D3DPOOL_DEFAULT, 0, 0, 0, NULL, NULL, false);

   if (!pass.tex)
      return false;

   d3d_set_texture(chain->dev, 0, pass.tex);
   d3d_set_sampler_address_u(chain->dev, 0, D3DTADDRESS_BORDER);
   d3d_set_sampler_address_v(chain->dev, 0, D3DTADDRESS_BORDER);
   d3d_set_texture(chain->dev, 0, NULL);

   pass_vector_list_append(chain->passes, pass);

   d3d9_cg_renderchain_log_info(chain, info);
   return true;
}

static bool d3d9_cg_renderchain_add_lut(void *data,
      const char *id, const char *path, bool smooth)
{
   struct lut_info info;
   cg_renderchain_t *chain = (cg_renderchain_t*)data;
   LPDIRECT3DTEXTURE9 lut  = (LPDIRECT3DTEXTURE9)
      d3d_texture_new(
         chain->dev,
         path,
         D3D_DEFAULT_NONPOW2,
         D3D_DEFAULT_NONPOW2,
         0,
         0,
         ((D3DFORMAT)-3), /* D3DFMT_FROM_FILE */
         D3DPOOL_MANAGED,
         smooth ? D3D_FILTER_LINEAR : D3D_FILTER_POINT,
         0,
         0,
         NULL,
         NULL,
         false
         );

   RARCH_LOG("[D3D]: LUT texture loaded: %s.\n", path);

   info.tex    = lut;
   info.smooth = smooth;
   strlcpy(info.id, id, sizeof(info.id));
   if (!lut)
      return false;

   d3d_set_texture(chain->dev, 0, lut);
   d3d_set_sampler_address_u(chain->dev, 0, D3DTADDRESS_BORDER);
   d3d_set_sampler_address_v(chain->dev, 0, D3DTADDRESS_BORDER);
   d3d_set_texture(chain->dev, 0, NULL);

   lut_info_vector_list_append(chain->luts, info);

   return true;
}

static void d3d9_cg_renderchain_add_state_tracker(
      void *data, void *tracker_data)
{
   state_tracker_t *tracker = (state_tracker_t*)tracker_data;
   cg_renderchain_t     *chain = (cg_renderchain_t*)data;
   if (chain->state_tracker)
      state_tracker_free(chain->state_tracker);
   chain->state_tracker = tracker;
}

static void d3d9_cg_renderchain_start_render(cg_renderchain_t *chain)
{
   chain->passes->data[0].tex         = chain->prev.tex[chain->prev.ptr];
   chain->passes->data[0].vertex_buf  = chain->prev.vertex_buf[chain->prev.ptr];
   chain->passes->data[0].last_width  = chain->prev.last_width[chain->prev.ptr];
   chain->passes->data[0].last_height = chain->prev.last_height[chain->prev.ptr];
}

static void d3d9_cg_renderchain_end_render(cg_renderchain_t *chain)
{
   chain->prev.last_width[chain->prev.ptr]  = chain->passes->data[0].last_width;
   chain->prev.last_height[chain->prev.ptr] = chain->passes->data[0].last_height;
   chain->prev.ptr                          = (chain->prev.ptr + 1) & TEXTURESMASK;
}

static void d3d9_cg_renderchain_set_shader_mvp(
      cg_renderchain_t *chain, CGprogram vPrg, D3DMATRIX *matrix)
{
   CGparameter cgpModelViewProj = cgGetNamedParameter(vPrg, "modelViewProj");
   if (cgpModelViewProj)
      cgD3D9SetUniformMatrix(cgpModelViewProj, matrix);
}

static void d3d9_cg_renderchain_calc_and_set_shader_mvp(
      cg_renderchain_t *chain, CGprogram vPrg,
      unsigned vp_width, unsigned vp_height,
      unsigned rotation)
{
   D3DMATRIX proj, ortho, rot, matrix;

   d3d_matrix_ortho_off_center_lh(&ortho, 0, vp_width, 0, vp_height, 0, 1);
   d3d_matrix_identity(&rot);
   d3d_matrix_rotation_z(&rot, rotation * (D3D_PI / 2.0));

   d3d_matrix_multiply(&proj, &ortho, &rot);
   d3d_matrix_transpose(&matrix, &proj);

   d3d9_cg_renderchain_set_shader_mvp(chain, vPrg, &matrix);
}

static void cg_d3d9_renderchain_set_vertices(
      cg_renderchain_t *chain,
      struct Pass *pass,
      unsigned width, unsigned height,
      unsigned out_width, unsigned out_height,
      unsigned vp_width, unsigned vp_height,
      unsigned rotation)
{
   const struct LinkInfo *info = (const struct LinkInfo*)&pass->info;

   if (pass->last_width != width || pass->last_height != height)
   {
      struct CGVertex vert[4];
      unsigned i;
      void *verts       = NULL;
      float _u          = (float)(width)  / info->tex_w;
      float _v          = (float)(height) / info->tex_h;

      pass->last_width  = width;
      pass->last_height = height;

      vert[0].x         = 0.0f;
      vert[0].y         = out_height;
      vert[0].z         = 0.5f;
      vert[0].u         = 0.0f;
      vert[0].v         = 0.0f;
      vert[0].lut_u     = 0.0f;
      vert[0].lut_v     = 0.0f;
      vert[0].r         = 1.0f;
      vert[0].g         = 1.0f;
      vert[0].b         = 1.0f;
      vert[0].a         = 1.0f;

      vert[1].x         = out_width;
      vert[1].y         = out_height;
      vert[1].z         = 0.5f;
      vert[1].u         = _u;
      vert[1].v         = 0.0f;
      vert[1].lut_u     = 1.0f;
      vert[1].lut_v     = 0.0f;
      vert[1].r         = 1.0f;
      vert[1].g         = 1.0f;
      vert[1].b         = 1.0f;
      vert[1].a         = 1.0f;

      vert[2].x         = 0.0f;
      vert[2].y         = 0.0f;
      vert[2].z         = 0.5f;
      vert[2].u         = 0.0f;
      vert[2].v         = _v;
      vert[2].lut_u     = 0.0f;
      vert[2].lut_v     = 1.0f;
      vert[2].r         = 1.0f;
      vert[2].g         = 1.0f;
      vert[2].b         = 1.0f;
      vert[2].a         = 1.0f;

      vert[3].x         = out_width;
      vert[3].y         = 0.0f;
      vert[3].z         = 0.5f;
      vert[3].u         = _u;
      vert[3].v         = _v;
      vert[3].lut_u     = 1.0f;
      vert[3].lut_v     = 1.0f;
      vert[3].r         = 1.0f;
      vert[3].g         = 1.0f;
      vert[3].b         = 1.0f;
      vert[3].a         = 1.0f;

      /* Align texels and vertices.
       *
       * Fixes infamous 'half-texel offset' issue of D3D9
       *	http://msdn.microsoft.com/en-us/library/bb219690%28VS.85%29.aspx.
       */
      for (i = 0; i < 4; i++)
      {
         vert[i].x     -= 0.5f;
         vert[i].y     += 0.5f;
      }

      verts             = d3d_vertex_buffer_lock(pass->vertex_buf);
      memcpy(verts, vert, sizeof(vert));
      d3d_vertex_buffer_unlock(pass->vertex_buf);
   }

   if (chain)
   {
      d3d9_cg_renderchain_calc_and_set_shader_mvp(
            chain, pass->vPrg, vp_width, vp_height, rotation);
      if (pass)
         d3d9_cg_renderchain_set_shader_params(chain, pass,
               width, height,
               info->tex_w, info->tex_h,
               vp_width, vp_height);
   }
}

static void cg_d3d9_renderchain_blit_to_texture(
      cg_renderchain_t *chain,
      const void *frame,
      unsigned width, unsigned height,
      unsigned pitch)
{
   D3DLOCKED_RECT d3dlr;
   struct Pass *first = (struct Pass*)&chain->passes->data[0];

   if (
         (first->last_width != width || first->last_height != height)
      )
   {
      d3d_lock_rectangle(first->tex, 0, &d3dlr,
            NULL, first->info.tex_h, D3DLOCK_NOSYSLOCK);
      d3d_lock_rectangle_clear(first->tex, 0, &d3dlr,
            NULL, first->info.tex_h, D3DLOCK_NOSYSLOCK);
   }

   if (d3d_lock_rectangle(first->tex, 0, &d3dlr, NULL, 0, 0))
   {
      d3d_texture_blit(chain->pixel_size, first->tex,
            &d3dlr, frame, width, height, pitch);
      d3d_unlock_rectangle(first->tex);
   }
}

static void cg_d3d9_renderchain_unbind_all(cg_renderchain_t *chain)
{
   unsigned i;

   /* Have to be a bit anal about it.
    * Render targets hate it when they have filters apparently.
    */
   for (i = 0; i < chain->bound_tex->count; i++)
   {
      d3d_set_sampler_minfilter(chain->dev,
            chain->bound_tex->data[i], D3DTEXF_POINT);
      d3d_set_sampler_magfilter(chain->dev,
            chain->bound_tex->data[i], D3DTEXF_POINT);
      d3d_set_texture(chain->dev, chain->bound_tex->data[i], NULL);
   }

   for (i = 0; i < chain->bound_vert->count; i++)
      d3d_set_stream_source(chain->dev, chain->bound_vert->data[i], 0, 0, 0);

   if (chain->bound_tex)
   {
      unsigned_vector_list_free(chain->bound_tex);
      chain->bound_tex = unsigned_vector_list_new();
   }

   if (chain->bound_vert)
   {
      unsigned_vector_list_free(chain->bound_vert);
      chain->bound_vert = unsigned_vector_list_new();
   }
}

static void cg_d3d9_renderchain_set_params(
      cg_renderchain_t *chain,
      struct Pass *pass,
      unsigned pass_index)
{
   unsigned i;

   /* Set state parameters. */
   if (chain->state_tracker)
   {
      /* Only query uniforms in first pass. */
      static struct state_tracker_uniform tracker_info[GFX_MAX_VARIABLES];
      static unsigned cnt = 0;

      if (pass_index == 1)
         cnt = state_tracker_get_uniform(chain->state_tracker, tracker_info,
               GFX_MAX_VARIABLES, chain->frame_count);

      for (i = 0; i < cnt; i++)
      {
         CGparameter param_f = cgGetNamedParameter(
               pass->fPrg, tracker_info[i].id);
         CGparameter param_v = cgGetNamedParameter(
               pass->vPrg, tracker_info[i].id);
         d3d9_cg_set_param_1f(param_f, &tracker_info[i].value);
         d3d9_cg_set_param_1f(param_v, &tracker_info[i].value);
      }
   }
}

static void cg_d3d9_renderchain_render_pass(
      cg_renderchain_t *chain,
      struct Pass *pass,
      unsigned pass_index)
{
   unsigned i, index;

   cgD3D9BindProgram(pass->fPrg);
   cgD3D9BindProgram(pass->vPrg);

   d3d_set_texture(chain->dev, 0, pass->tex);
   d3d_set_sampler_minfilter(chain->dev, 0,
         d3d_translate_filter(pass->info.pass->filter));
   d3d_set_sampler_magfilter(chain->dev, 0,
         d3d_translate_filter(pass->info.pass->filter));

   d3d_set_vertex_declaration(chain->dev, pass->vertex_decl);
   for (i = 0; i < 4; i++)
      d3d_set_stream_source(chain->dev, i,
            pass->vertex_buf, 0,
            sizeof(struct CGVertex));

   /* Set orig texture. */
   d3d9_cg_renderchain_bind_orig(chain, pass);

   /* Set prev textures. */
   d3d9_cg_renderchain_bind_prev(chain, (const void*)pass);

   /* Set lookup textures */
   for (i = 0; i < chain->luts->count; i++)
   {
      CGparameter vparam;
      CGparameter fparam = cgGetNamedParameter(
            pass->fPrg, chain->luts->data[i].id);
      int bound_index    = -1;

      if (fparam)
      {
         index           = cgGetParameterResourceIndex(fparam);
         bound_index     = index;

         d3d9_cg_renderchain_add_lut_internal(chain, index, i);
      }

      vparam             = cgGetNamedParameter(pass->vPrg, chain->luts->data[i].id);

      if (vparam)
      {
         index           = cgGetParameterResourceIndex(vparam);
         if (index != (unsigned)bound_index)
            d3d9_cg_renderchain_add_lut_internal(chain, index, i);
      }
   }

   d3d9_cg_renderchain_bind_pass(chain, pass, pass_index);

   cg_d3d9_renderchain_set_params(chain, pass, pass_index);

   d3d_draw_primitive(chain->dev, D3DPT_TRIANGLESTRIP, 0, 2);

   /* So we don't render with linear filter into render targets,
    * which apparently looked odd (too blurry). */
   d3d_set_sampler_minfilter(chain->dev, 0, D3DTEXF_POINT);
   d3d_set_sampler_magfilter(chain->dev, 0, D3DTEXF_POINT);

   cg_d3d9_renderchain_unbind_all(chain);
}

static bool d3d9_cg_renderchain_render(
      void *data,
      const void *frame_data,
      unsigned width, unsigned height,
      unsigned pitch, unsigned rotation)
{
   LPDIRECT3DSURFACE9 back_buffer, target;
   unsigned i, current_width, current_height, out_width = 0, out_height = 0;
   struct Pass *last_pass  = NULL;
   d3d_video_t *d3d        = (d3d_video_t*)data;
   cg_renderchain_t *chain = d3d ? (cg_renderchain_t*)d3d->renderchain_data : NULL;

   d3d9_cg_renderchain_start_render(chain);

   current_width         = width;
   current_height        = height;
   d3d9_cg_renderchain_convert_geometry(chain, &chain->passes->data[0].info,
         &out_width, &out_height,
         current_width, current_height, chain->final_viewport);

   cg_d3d9_renderchain_blit_to_texture(chain,
         frame_data, width, height, pitch);

   /* Grab back buffer. */
   d3d_device_get_render_target(chain->dev, 0, (void**)&back_buffer);

   /* In-between render target passes. */
   for (i = 0; i < chain->passes->count - 1; i++)
   {
      D3DVIEWPORT9   viewport = {0};
      struct Pass *from_pass  = (struct Pass*)&chain->passes->data[i];
      struct Pass *to_pass    = (struct Pass*)&chain->passes->data[i + 1];

      d3d_texture_get_surface_level(to_pass->tex, 0, (void**)&target);

      d3d_device_set_render_target(chain->dev, 0, (void*)target);

      d3d9_cg_renderchain_convert_geometry(chain, &from_pass->info,
            &out_width, &out_height,
            current_width, current_height, chain->final_viewport);

      /* Clear out whole FBO. */
      viewport.Width  = to_pass->info.tex_w;
      viewport.Height = to_pass->info.tex_h;
      viewport.MinZ   = 0.0f;
      viewport.MaxZ   = 1.0f;

      d3d_set_viewports(chain->dev, &viewport);
      d3d_clear(chain->dev, 0, 0, D3DCLEAR_TARGET, 0, 1, 0);

      viewport.Width  = out_width;
      viewport.Height = out_height;

      d3d_set_viewports(chain->dev, &viewport);

      cg_d3d9_renderchain_set_vertices(chain, from_pass,
            current_width, current_height,
            out_width, out_height,
            out_width, out_height, 0);

      if (chain)
         cg_d3d9_renderchain_render_pass(chain, from_pass, i + 1);

      current_width = out_width;
      current_height = out_height;
      d3d_surface_free(target);
   }

   /* Final pass */
   d3d_device_set_render_target(chain->dev, 0, (void*)back_buffer);

   last_pass = (struct Pass*)&chain->passes->
      data[chain->passes->count - 1];

   d3d9_cg_renderchain_convert_geometry(chain, &last_pass->info,
         &out_width, &out_height,
         current_width, current_height, chain->final_viewport);

   d3d_set_viewports(chain->dev, chain->final_viewport);

   cg_d3d9_renderchain_set_vertices(chain, last_pass,
         current_width, current_height,
         out_width, out_height,
         chain->final_viewport->Width, chain->final_viewport->Height,
         rotation);

   if (chain)
      cg_d3d9_renderchain_render_pass(chain,
            last_pass, chain->passes->count);

   chain->frame_count++;

   d3d_surface_free(back_buffer);

   if (chain)
   {
      d3d9_cg_renderchain_end_render(chain);
      cgD3D9BindProgram(chain->fStock);
      cgD3D9BindProgram(chain->vStock);
      d3d9_cg_renderchain_calc_and_set_shader_mvp(
            chain, chain->vStock, chain->final_viewport->Width,
            chain->final_viewport->Height, 0);
   }

   return true;
}

static void d3d9_cg_renderchain_set_font_rect(
      void *data,
      const void *font_data)
{
   settings_t *settings             = config_get_ptr();
   d3d_video_t *d3d                 = (d3d_video_t*)data;
   float pos_x                      = settings->floats.video_msg_pos_x;
   float pos_y                      = settings->floats.video_msg_pos_y;
   float font_size                  = settings->floats.video_font_size;
   const struct font_params *params = (const struct font_params*)font_data;

   if (params)
   {
      pos_x                       = params->x;
      pos_y                       = params->y;
      font_size                  *= params->scale;
   }

   if (!d3d)
      return;

   d3d->font_rect.left            = d3d->video_info.width * pos_x;
   d3d->font_rect.right           = d3d->video_info.width;
   d3d->font_rect.top             = (1.0f - pos_y) * d3d->video_info.height - font_size;
   d3d->font_rect.bottom          = d3d->video_info.height;

   d3d->font_rect_shifted         = d3d->font_rect;
   d3d->font_rect_shifted.left   -= 2;
   d3d->font_rect_shifted.right  -= 2;
   d3d->font_rect_shifted.top    += 2;
   d3d->font_rect_shifted.bottom += 2;
}

static bool d3d9_cg_renderchain_read_viewport(
      void *data, uint8_t *buffer, bool is_idle)
{
   unsigned width, height;
   D3DLOCKED_RECT rect;
   LPDIRECT3DSURFACE9 target = NULL;
   LPDIRECT3DSURFACE9 dest   = NULL;
   bool ret                  = true;
   d3d_video_t *d3d          = (d3d_video_t*)data;
   LPDIRECT3DDEVICE9 d3dr    = (LPDIRECT3DDEVICE9)d3d->dev;

   video_driver_get_size(&width, &height);

   (void)d3d;
   (void)data;
   (void)buffer;

   if (
         !d3d_device_get_render_target(d3dr, 0, (void**)&target)     ||
         !d3d_device_create_offscreen_plain_surface(d3dr, width, height,
            d3d_get_xrgb8888_format(),
            D3DPOOL_SYSTEMMEM, (void**)&dest, NULL) ||
         !d3d_device_get_render_target_data(d3dr, (void*)target, (void*)dest)
         )
   {
      ret = false;
      goto end;
   }

   if (d3d_surface_lock_rect(dest, (void*)&rect))
   {
      unsigned x, y;
      unsigned pitchpix       = rect.Pitch / 4;
      const uint32_t *pixels  = (const uint32_t*)rect.pBits;

      pixels                 += d3d->final_viewport.x;
      pixels                 += (d3d->final_viewport.height - 1) * pitchpix;
      pixels                 -= d3d->final_viewport.y * pitchpix;

      for (y = 0; y < d3d->final_viewport.height; y++, pixels -= pitchpix)
      {
         for (x = 0; x < d3d->final_viewport.width; x++)
         {
            *buffer++ = (pixels[x] >>  0) & 0xff;
            *buffer++ = (pixels[x] >>  8) & 0xff;
            *buffer++ = (pixels[x] >> 16) & 0xff;
         }
      }

      d3d_surface_unlock_rect((void*)dest);
   }
   else
      ret = false;

end:
   if (target)
      d3d_surface_free(target);
   if (dest)
      d3d_surface_free(dest);
   return ret;
}

static void d3d9_cg_renderchain_viewport_info(
      void *data, struct video_viewport *vp)
{
   unsigned width, height;
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (!d3d || !vp)
      return;

   video_driver_get_size(&width, &height);

   vp->x            = d3d->final_viewport.x;
   vp->y            = d3d->final_viewport.y;
   vp->width        = d3d->final_viewport.width;
   vp->height       = d3d->final_viewport.height;

   vp->full_width   = width;
   vp->full_height  = height;
}

d3d_renderchain_driver_t cg_d3d9_renderchain = {
   d3d9_cg_renderchain_free,
   d3d9_cg_renderchain_new,
   d3d9_cg_renderchain_init,
   d3d9_cg_renderchain_set_final_viewport,
   d3d9_cg_renderchain_add_pass,
   d3d9_cg_renderchain_add_lut,
   d3d9_cg_renderchain_add_state_tracker,
   d3d9_cg_renderchain_render,
   d3d9_cg_renderchain_convert_geometry,
   d3d9_cg_renderchain_set_font_rect,
   d3d9_cg_renderchain_read_viewport,
   d3d9_cg_renderchain_viewport_info,
   "cg_d3d9",
};
