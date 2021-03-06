/*
 * This file is part of SpatialCL, a library for the spatial processing of
 * particles.
 *
 * Copyright (c) 2017, 2018 Aksel Alpay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef QUERY_ENGINE_DFS_HPP
#define QUERY_ENGINE_DFS_HPP

#include <QCL/qcl.hpp>
#include <QCL/qcl_module.hpp>

#include "../configuration.hpp"
#include "../tree/binary_tree.hpp"


namespace spatialcl {
namespace query {
namespace engine {

enum depth_first_iteration_strategy
{
  HIERARCHICAL_ITERATION_STRICT = 0,
  HIERARCHICAL_ITERATION_RELAXED = 1
};

/// Depth-first query.
/// \tparam Tree_type the tree type on which this query operates
/// \tparam Handler_module A query handler, fulfilling the dfs handler concept
/// \tparam group_size The OpenCL group size of the query. A 0 will correspond
/// to a cl::NullRange and will hence allow the OpenCL implementation to choose
/// the group size
template<class Tree_type,
         class Handler_module,
         depth_first_iteration_strategy Iteration_strategy,
         std::size_t group_size = 256>
class depth_first
{
public:
  QCL_MAKE_MODULE(depth_first)

  using handler_type = Handler_module;
  using type_system = typename Tree_type::type_system;

  /// Execute query
  cl_int operator()(const Tree_type& tree,
                    Handler_module& handler,
                    cl::Event* evt = nullptr)
  {
    return this->run(tree.get_device_context(),
                     tree.get_sorted_particles(),
                     tree.get_node_values0(),
                     tree.get_node_values1(),
                     tree.get_num_particles(),
                     tree.get_effective_num_particles(),
                     tree.get_effective_num_levels(),
                     handler,
                     evt);
  }
private:
  cl_int run(const qcl::device_context_ptr& ctx,
             const cl::Buffer& particles,
             const cl::Buffer& node_values0,
             const cl::Buffer& node_values1,
             std::size_t num_particles,
             std::size_t effective_num_particles,
             std::size_t effective_num_levels,
             Handler_module& handler,
             cl::Event* evt = nullptr)
  {

    cl::NDRange local_size = cl::NullRange;
    if(group_size > 0)
      local_size = cl::NDRange{group_size};

    qcl::kernel_call call = query(ctx,
                                  cl::NDRange{handler.get_num_independent_queries()},
                                  local_size,
                                  evt);

    call.partial_argument_list(particles,
                               node_values0,
                               node_values1,
                               static_cast<cl_ulong>(num_particles),
                               static_cast<cl_ulong>(effective_num_particles),
                               static_cast<cl_ulong>(effective_num_levels));

    handler.push_full_arguments(call);
    return call.enqueue_kernel();
  }

  QCL_ENTRYPOINT(query)
  QCL_MAKE_SOURCE(
    QCL_INCLUDE_MODULE(tree_configuration<Tree_type>)
    QCL_INCLUDE_MODULE(Handler_module)
    QCL_INCLUDE_MODULE(binary_tree)
    QCL_IMPORT_CONSTANT(Iteration_strategy)
    QCL_IMPORT_CONSTANT(group_size)
    QCL_RAW(
        ulong load_node(binary_tree_key_t* node,
                       __global node_type0* node_values0,
                       __global node_type1* node_values1,
                       ulong effective_num_levels,
                       ulong effective_num_particles,
                       node_type0* node_value0_out,
                       node_type1* node_value1_out)
        {
          ulong idx = binary_tree_key_encode_global_id(node,effective_num_levels);
          idx -= effective_num_particles;

          *node_value0_out = node_values0[idx];
          *node_value1_out = node_values1[idx];

          return idx;
        }

        ulong load_particle(binary_tree_key_t* node,
                       __global particle_type* particles,
                       ulong effective_num_levels,
                       ulong effective_num_particles,
                       particle_type* particle_out)
        {

          // Since particles are at the lowest level, we know that for them
          // the index equals the local node id
          ulong idx = node->local_node_id;
          *particle_out = particles[idx];
          return idx;
        }

        binary_tree_key_t find_first_left_parent(binary_tree_key_t* node)
        {
          binary_tree_key_t result = binary_tree_get_parent(node);
          while(binary_tree_is_right_child(&result))
            result = binary_tree_get_parent(&result);
          return result;
        }
      )
      R"(
      #if Iteration_strategy == 0
        // Strict iteration
        #define NEXT_PARENT(node) find_first_left_parent(&node)
      #elif Iteration_strategy == 1
        // Relaxed iteration
        #define NEXT_PARENT(node) binary_tree_get_parent(&node)
      #else
        #error Invalid iteration strategy
      #endif

      #if group_size > 0
        #define KERNEL_ATTRIBUTES __attribute__((reqd_work_group_size(group_size,1,1)))
      #else
        #define KERNEL_ATTRIBUTES
      #endif
      )"
      QCL_PREPROCESSOR(define, get_query_id() tid)
      QCL_PREPROCESSOR(define,
        QUERY_NODE_LEVEL(node_values0,
                         node_values1,
                         effective_num_particles,
                         effective_num_levels,
                         current_node,
                         num_covered_particles)
        {
          node_type0 current_node_values0;
          node_type1 current_node_values1;

          ulong node_idx = load_node(&current_node,
                                     node_values0,
                                     node_values1,
                                     effective_num_levels,
                                     effective_num_particles,
                                     &current_node_values0,
                                     &current_node_values1);

          int node_selected = 0;
          dfs_node_selector(&node_selected,
                            &current_node,
                            node_idx,
                            current_node_values0,
                            current_node_values1);
          if(node_selected)
          {
            current_node = binary_tree_get_children_begin(&current_node);
          }
          else
          {
            dfs_unique_node_discard_event(node_idx,
                                          current_node_values0,
                                          current_node_values1);

            num_covered_particles += BT_LEAVES_PER_NODE(current_node.level,
                                                        effective_num_levels);

            if(binary_tree_is_right_child(&current_node))
            {
              // if we are at a right child node, go up to the parent's
              // sibling...
              current_node = NEXT_PARENT(current_node);
              current_node.local_node_id++;
            }
            else
              // otherwise, first investigate the sibling
              current_node.local_node_id++;
          }
        }
      )
      QCL_PREPROCESSOR(define,
        QUERY_PARTICLE_LEVEL(particles,
                             effective_num_particles,
                             effective_num_levels,
                             current_node,
                             num_covered_particles)
        {
          particle_type current_particle;

          ulong particle_idx = load_particle(&current_node,
                                             particles,
                                             effective_num_levels,
                                             effective_num_particles,
                                             &current_particle);

          int particle_selected = 0;
          dfs_particle_processor(&particle_selected,
                                 particle_idx,
                                 current_particle);
          if(particle_selected)
          {
            // Move to next particle
            current_node.local_node_id++;
          }
          else
          {
            if(binary_tree_is_right_child(&current_node))
            {
              // if we are at a right child node, go up to the parent's
              // sibling...
              current_node = NEXT_PARENT(current_node);
              current_node.local_node_id++;
            }
            else
              // otherwise, first investigate the sibling
              current_node.local_node_id++;
          }
          num_covered_particles++;
        }
      )
      QCL_RAW(
        __kernel void query(__global particle_type* particles,
                            __global node_type0* node_values0,
                            __global node_type1* node_values1,
                            ulong num_particles,
                            ulong effective_num_particles,
                            ulong effective_num_levels,
                            declare_full_query_parameter_set())
          KERNEL_ATTRIBUTES
        {
          for(size_t tid = get_global_id(0);
              tid < get_num_queries();
              tid += get_global_size(0))
          {
            at_query_init();

            binary_tree_key_t current_node;
            current_node.level = 0;
            current_node.local_node_id = 0;

            ulong num_covered_particles = 0;
            while(num_covered_particles < num_particles)
            {
              int particle_level_reached = (current_node.level == effective_num_levels-1);

              if(particle_level_reached)
              {
                QUERY_PARTICLE_LEVEL(particles,
                                     effective_num_particles,
                                     effective_num_levels,
                                     current_node,
                                     num_covered_particles);
              }
              else
              {
                QUERY_NODE_LEVEL(node_values0,
                                 node_values1,
                                 effective_num_particles,
                                 effective_num_levels,
                                 current_node,
                                 num_covered_particles);
              }
            }

            at_query_exit();
          }
        }
      )
  )
};

}
}
}

#endif
