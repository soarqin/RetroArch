/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2016 - Hans-Kristian Arntzen
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

#include "spir2cross.hpp"
#include "slang_reflection.hpp"
#include <vector>
#include <stdio.h>
#include "../../verbosity.h"

using namespace std;
using namespace spir2cross;

static slang_texture_semantic slang_name_to_semantic(const string &name)
{
   if (name == "Original")
      return SLANG_TEXTURE_SEMANTIC_ORIGINAL;
   else if (name == "Source")
      return SLANG_TEXTURE_SEMANTIC_SOURCE;
   else
      return SLANG_TEXTURE_INVALID_SEMANTIC;
}

static bool find_uniform_offset(const Compiler &compiler, const Resource &resource, const char *name,
      size_t *offset, unsigned *member_index)
{
   auto &type = compiler.get_type(resource.type_id);
   size_t num_members = type.member_types.size();
   for (size_t i = 0; i < num_members; i++)
   {
      if (compiler.get_member_name(resource.type_id, i) == name)
      {
         *offset = compiler.get_member_decoration(resource.type_id, i, spv::DecorationOffset);
         *member_index = i;
         return true;
      }
   }
   return false;
}

static bool slang_reflect(const Compiler &vertex_compiler, const Compiler &fragment_compiler,
      const ShaderResources &vertex, const ShaderResources &fragment,
      slang_reflection *reflection)
{
   unsigned member_index = 0;

   // Validate use of unexpected types.
   if (
         !vertex.sampled_images.empty() ||
         !vertex.storage_buffers.empty() ||
         !vertex.subpass_inputs.empty() ||
         !vertex.storage_images.empty() ||
         !vertex.atomic_counters.empty() ||
         !vertex.push_constant_buffers.empty() ||
         !fragment.storage_buffers.empty() ||
         !fragment.subpass_inputs.empty() ||
         !fragment.storage_images.empty() ||
         !fragment.atomic_counters.empty() ||
         !fragment.push_constant_buffers.empty())
   {
      RARCH_ERR("Invalid resource type detected.\n");
      return false;
   }

   // Validate vertex input.
   if (vertex.stage_inputs.size() != 2)
   {
      RARCH_ERR("Vertex must have two attributes.\n");
      return false;
   }

   uint32_t location_mask = 0;
   for (auto &input : vertex.stage_inputs)
      location_mask |= 1 << vertex_compiler.get_decoration(input.id, spv::DecorationLocation);

   if (location_mask != 0x3)
   {
      RARCH_ERR("The two vertex attributes do not use location = 0 and location = 1.\n");
      return false;
   }

   // Validate the single uniform buffer.
   if (vertex.uniform_buffers.size() != 1)
   {
      RARCH_ERR("Vertex must use exactly one uniform buffer.\n");
      return false;
   }

   if (fragment.uniform_buffers.size() > 1)
   {
      RARCH_ERR("Fragment must use zero or one uniform buffer.\n");
      return false;
   }

   if (vertex_compiler.get_decoration(vertex.uniform_buffers[0].id, spv::DecorationDescriptorSet) != 0)
   {
      RARCH_ERR("Resources must use descriptor set #0.\n");
      return false;
   }

   if (!fragment.uniform_buffers.empty() &&
         fragment_compiler.get_decoration(fragment.uniform_buffers[0].id, spv::DecorationDescriptorSet) != 0)
   {
      RARCH_ERR("Resources must use descriptor set #0.\n");
      return false;
   }

   unsigned ubo_binding = vertex_compiler.get_decoration(vertex.uniform_buffers[0].id,
         spv::DecorationBinding);
   if (!fragment.uniform_buffers.empty() &&
         ubo_binding != fragment_compiler.get_decoration(fragment.uniform_buffers[0].id, spv::DecorationBinding))
   {
      RARCH_ERR("Vertex and fragment uniform buffer must have same binding.\n");
      return false;
   }

   if (ubo_binding >= SLANG_NUM_BINDINGS)
   {
      RARCH_ERR("Binding %u is out of range.\n", ubo_binding);
      return false;
   }

   reflection->ubo_stage_mask = SLANG_STAGE_VERTEX_MASK;
   reflection->ubo_size = vertex_compiler.get_declared_struct_size(
         vertex_compiler.get_type(vertex.uniform_buffers[0].type_id));

   if (!fragment.uniform_buffers.empty())
   {
      reflection->ubo_stage_mask |= SLANG_STAGE_FRAGMENT_MASK;
      reflection->ubo_size = max(reflection->ubo_size,
            fragment_compiler.get_declared_struct_size(fragment_compiler.get_type(fragment.uniform_buffers[0].type_id)));
   }

   if (!find_uniform_offset(vertex_compiler, vertex.uniform_buffers[0], "MVP", &reflection->mvp_offset, &member_index))
   {
      RARCH_ERR("Could not find offset for MVP matrix.\n");
      return false;
   }

   uint32_t binding_mask = 1 << ubo_binding;

   // On to textures.
   for (auto &texture : fragment.sampled_images)
   {
      unsigned set = fragment_compiler.get_decoration(texture.id, spv::DecorationDescriptorSet);
      unsigned binding = fragment_compiler.get_decoration(texture.id, spv::DecorationBinding);

      if (set != 0)
      {
         RARCH_ERR("Resources must use descriptor set #0.\n");
         return false;
      }

      if (binding >= SLANG_NUM_BINDINGS)
      {
         RARCH_ERR("Binding %u is out of range.\n", ubo_binding);
         return false;
      }

      if (binding_mask & (1 << binding))
      {
         RARCH_ERR("Binding %u is already in use.\n", binding);
         return false;
      }
      binding_mask |= 1 << binding;

      slang_texture_semantic index = slang_name_to_semantic(texture.name); 
      
      if (index == SLANG_TEXTURE_INVALID_SEMANTIC)
      {
         RARCH_ERR("Non-semantic textures not supported.\n");
         return false;
      }

      auto &semantic = reflection->semantic_textures[index];
      semantic.binding = binding;
      semantic.stage_mask = SLANG_STAGE_FRAGMENT_MASK;
      reflection->semantic_texture_mask |= 1 << index;

      // TODO: Do we want to expose Size uniforms if no stage is using the texture?
      char uniform_name[128];
      snprintf(uniform_name, sizeof(uniform_name), "%sSize", texture.name.c_str());
      if (find_uniform_offset(fragment_compiler, fragment.uniform_buffers[0], uniform_name,
               &semantic.ubo_offset, &member_index))
      {
         auto &type = fragment_compiler.get_type(
               fragment_compiler.get_type(fragment.uniform_buffers[0].type_id).member_types[member_index]);

         // Verify that the type is a vec4 to avoid any nasty surprises later.
         bool is_vec4 = type.basetype == SPIRType::Float && type.array.empty() && type.vecsize == 4 && type.columns == 1;
         if (!is_vec4)
         {
            RARCH_ERR("Semantic uniform is not vec4.\n");
            return false;
         }

         reflection->semantic_texture_ubo_mask |= 1 << index;
      }
   }

   return true;
}

bool slang_reflect_spirv(const std::vector<uint32_t> &vertex,
      const std::vector<uint32_t> &fragment,
      slang_reflection *reflection)
{
   try
   {
      Compiler vertex_compiler(vertex);
      Compiler fragment_compiler(fragment);
      auto vertex_resources = vertex_compiler.get_shader_resources();
      auto fragment_resources = fragment_compiler.get_shader_resources();

      if (!slang_reflect(vertex_compiler, fragment_compiler,
               vertex_resources, fragment_resources,
               reflection))
      {
         RARCH_ERR("Failed to reflect SPIR-V. Resource usage is inconsistent with expectations.\n");
         return false;
      }

      return true;
   }
   catch (const std::exception &e)
   {
      RARCH_ERR("spir2cross threw exception: %s.\n", e.what());
      return false;
   }
}

