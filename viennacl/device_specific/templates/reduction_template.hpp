#ifndef VIENNACL_DEVICE_SPECIFIC_TEMPLATES_REDUCTION_TEMPLATE_HPP
#define VIENNACL_DEVICE_SPECIFIC_TEMPLATES_REDUCTION_TEMPLATE_HPP

/* =========================================================================
   Copyright (c) 2010-2013, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */


/** @file viennacl/generator/scalar_reduction.hpp
 *
 * Kernel template for the scalar reduction operation
*/

#include <vector>

#include "viennacl/backend/opencl.hpp"

#include "viennacl/scheduler/forwards.h"
#include "viennacl/device_specific/tree_parsing/read_write.hpp"
#include "viennacl/device_specific/tree_parsing/evaluate_expression.hpp"
#include "viennacl/device_specific/utils.hpp"

#include "viennacl/device_specific/templates/template_base.hpp"
#include "viennacl/device_specific/templates/reduction_utils.hpp"

#include "viennacl/tools/tools.hpp"

namespace viennacl{

  namespace device_specific{

    class reduction_template : public template_base{
      typedef std::vector<std::pair<const char *, viennacl::ocl::handle<cl_mem> > > temporaries_type;

    public:
      /** @brief The user constructor */
      reduction_template(const char * scalartype, unsigned int simd_width, unsigned int local_size, unsigned int num_groups, unsigned int decomposition) : template_base(scalartype, simd_width, local_size, 1, 2), num_groups_(num_groups), decomposition_(decomposition){ }

      void configure_range_enqueue_arguments(unsigned int kernel_id, statements_container const & statements, viennacl::ocl::kernel & k, unsigned int & n_arg)  const{
        //create temporaries
        init_temporaries(statements);

        //configure ND range
        if(kernel_id==0){
          configure_local_sizes(k, 0);
          unsigned int gsize = local_size_0_*num_groups_;
          k.global_work_size(0,gsize);
          k.global_work_size(1,1);
        }
        else{
          configure_local_sizes(k, 1);
          k.global_work_size(0,local_size_0_);
          k.global_work_size(1,1);
        }

        //set arguments
        cl_uint size = get_vector_size(statements.data().front());
        k.arg(n_arg++, size/simd_width_);
        for(temporaries_type::iterator it = temporaries_.begin() ; it != temporaries_.end() ; ++it)
          k.arg(n_arg++, it->second);
      }

      void add_kernel_arguments(statements_container const & statements, std::string & arguments_string) const{
        init_temporaries(statements);
        arguments_string += generate_value_kernel_argument("unsigned int", "N");
        for(temporaries_type::iterator it = temporaries_.begin() ; it != temporaries_.end() ; ++it){
          arguments_string += generate_pointer_kernel_argument("__global", it->first, "temp" + tools::to_string(std::distance(temporaries_.begin(), it)));
        }
      }

    private:
      unsigned int lmem_used(unsigned int scalartype_size) const {
        return local_size_0_*scalartype_size;
      }

      void init_temporaries(statements_container const & statements) const {
        if(temporaries_.empty()){
          //set temporary buffer argument
          for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it){
            scheduler::statement::container_type const & array = it->array();
            unsigned int size_of_scalartype;
            const char * scalartype_name;
            switch(array[0].lhs.numeric_type){
            case scheduler::FLOAT_TYPE: scalartype_name = "float"; size_of_scalartype = sizeof(float); break;
            case scheduler::DOUBLE_TYPE: scalartype_name = "double"; size_of_scalartype = sizeof(double); break;
            default : throw generator_not_supported_exception("Unsupported scalartype");
            }
            for(scheduler::statement::container_type::const_iterator iit = array.begin() ; iit != array.end() ; ++iit){
              if(is_scalar_reduction(*iit)){
                temporaries_.push_back(std::make_pair(scalartype_name, viennacl::ocl::current_context().create_memory(CL_MEM_READ_WRITE, num_groups_*size_of_scalartype)));
              }
            }
          }
        }
      }

      void fill_scalartypes(statements_container const & statements, std::vector<const char *> & res) const {
        res.reserve(statements.data().size());
        for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it){
          scheduler::lhs_rhs_element const & lhs = it->array()[it->root()].lhs;
          if (lhs.type_family == scheduler::SCALAR_TYPE_FAMILY)
          {
            switch(lhs.numeric_type){
            case scheduler::FLOAT_TYPE:
              res.push_back("float");
              break;
            case scheduler::DOUBLE_TYPE:
              res.push_back("double");
              break;
            default:
              res.push_back("");
              break;
            }
          }
          else
          {
            res.push_back("");
          }
        }
      }

      void core_0(utils::kernel_generation_stream& stream, std::vector<mapped_scalar_reduction*> exprs, std::vector<const char *> const & scalartypes, statements_container const & statements, std::vector<mapping_type> const & /*mapping*/) const {
        unsigned int N = exprs.size();

        std::vector<scheduler::op_element> rops(N);
        std::vector<std::string> accs(N);
        std::vector<std::string> local_buffers_names(N);
        for(unsigned int k = 0 ; k < N ; ++k){
          scheduler::op_element root_op = exprs[k]->statement().array()[exprs[k]->root_idx()].op;
          rops[k].type_family = scheduler::OPERATION_BINARY_TYPE_FAMILY;
          if(root_op.type==scheduler::OPERATION_BINARY_INNER_PROD_TYPE){
            rops[k].type        = scheduler::OPERATION_BINARY_ADD_TYPE;
          }
          else{
            rops[k].type        = root_op.type;
          }
          accs[k] = "acc"+tools::to_string(k);
          local_buffers_names[k] = "buf"+tools::to_string(k);
        }

        stream << "unsigned int lid = get_local_id(0);" << std::endl;

        for(unsigned int k = 0 ; k < N ; ++k)
          stream << scalartypes[k] << " " << accs[k] << " = " << neutral_element(rops[k]) << ";" << std::endl;

        std::string init;
        std::string upper_bound;
        std::string inc;
        if(decomposition_){
          init = "get_global_id(0)";
          upper_bound = "N";
          inc = "get_global_size(0)";
        }
        else{
          stream << "unsigned int chunk_size = (N + get_num_groups(0)-1)/get_num_groups(0);" << std::endl;
          stream << "unsigned int chunk_start = get_group_id(0)*chunk_size;" << std::endl;
          stream << "unsigned int chunk_end = min(chunk_start+chunk_size, N);" << std::endl;
          init = "chunk_start + get_local_id(0)";
          upper_bound = "chunk_end";
          inc = "get_local_size(0)";
        }

        stream << "for(unsigned int i = " << init << "; i < " << upper_bound << " ; i += " << inc << "){" << std::endl;
        stream.inc_tab();
        {
          //Fetch vector entry
          std::set<std::string>  cache;
          for(std::vector<mapped_scalar_reduction*>::iterator it = exprs.begin() ; it != exprs.end() ; ++it)
          {
            tree_parsing::read_write(&mapped_handle::fetch, "reg", cache, (*it)->statement(), (*it)->root_idx(), index_tuple("i", "N"),stream,(*it)->mapping(), tree_parsing::PARENT_NODE_TYPE);
          }
          //Update accs;
          for(unsigned int k = 0 ; k < exprs.size() ; ++k)
          {
            viennacl::scheduler::statement const & statement = exprs[k]->statement();
            unsigned int root_idx = exprs[k]->root_idx();
            mapping_type const & mapping = exprs[k]->mapping();
            if(simd_width_ > 1){
              for(unsigned int a = 0 ; a < simd_width_ ; ++a){
                std::string str;
                tree_parsing::generate_all_lhs(statement,root_idx,index_tuple("i","N"),a,str,mapping);
                if(statement.array()[root_idx].op.type==scheduler::OPERATION_BINARY_INNER_PROD_TYPE){
                  str += "*";
                  tree_parsing::generate_all_rhs(statement,root_idx,index_tuple("i","N"),a,str,mapping);
                }
                compute_reduction(stream,accs[k],str,rops[k]);
              }
            }
            else{
              std::string str;
              tree_parsing::generate_all_lhs(statement,root_idx,index_tuple("i","N"),-1,str,mapping);
              if(statement.array()[root_idx].op.type==scheduler::OPERATION_BINARY_INNER_PROD_TYPE){
                str += "*";
                tree_parsing::generate_all_rhs(statement,root_idx,index_tuple("i","N"),-1,str,mapping);
              }
              compute_reduction(stream,accs[k],str,rops[k]);
            }
          }
        }
        stream.dec_tab();
        stream << "}" << std::endl;


        //Declare and fill local memory
        for(unsigned int k = 0 ; k < N ; ++k)
          stream << "__local " << scalartypes[k] << " " << local_buffers_names[k] << "[" << local_size_0_ << "];" << std::endl;

        for(unsigned int k = 0 ; k < N ; ++k)
          stream << local_buffers_names[k] << "[lid] = " << accs[k] << ";" << std::endl;

        //Reduce and write to temporary buffers
        reduce_1d_local_memory(stream, local_size_0_,local_buffers_names,rops);

        stream << "if(lid==0){" << std::endl;
        stream.inc_tab();
        for(unsigned int k = 0 ; k < N ; ++k)
          stream << "temp"<< k << "[get_group_id(0)] = buf" << k << "[0];" << std::endl;
        stream.dec_tab();
        stream << "}" << std::endl;
      }


      void core_1(utils::kernel_generation_stream& stream, std::vector<mapped_scalar_reduction*> exprs, std::vector<const char *> scalartypes, statements_container const & statements, std::vector<mapping_type> const & mapping) const {
        unsigned int N = exprs.size();
        std::vector<scheduler::op_element> rops(N);
        std::vector<std::string> accs(N);
        std::vector<std::string> local_buffers_names(N);
        for(unsigned int k = 0 ; k < N ; ++k){
          scheduler::op_element root_op = exprs[k]->statement().array()[exprs[k]->root_idx()].op;
          rops[k].type_family = scheduler::OPERATION_BINARY_TYPE_FAMILY;
          if(root_op.type==scheduler::OPERATION_BINARY_INNER_PROD_TYPE){
            rops[k].type        = scheduler::OPERATION_BINARY_ADD_TYPE;
          }
          else{
            rops[k].type        = root_op.type;
          }
          accs[k] = "acc"+tools::to_string(k);
          local_buffers_names[k] = "buf"+tools::to_string(k);
        }



        stream << "unsigned int lid = get_local_id(0);" << std::endl;

        for(unsigned int k = 0 ; k < exprs.size() ; ++k)
          stream << "__local " << scalartypes[k] << " " << local_buffers_names[k] << "[" << local_size_0_ << "];" << std::endl;

        for(unsigned int k = 0 ; k < local_buffers_names.size() ; ++k)
          stream << scalartypes[0] << " " << accs[k] << " = " << neutral_element(rops[k]) << ";" << std::endl;

        stream << "for(unsigned int i = lid ; i < " << num_groups_ << " ; i += get_local_size(0)){" << std::endl;
        stream.inc_tab();
        for(unsigned int k = 0 ; k < N ; ++k)
          compute_reduction(stream,accs[k],"temp"+tools::to_string(k)+"[i]",rops[k]);
        stream.dec_tab();
        stream << "}" << std::endl;

        for(unsigned int k = 0 ; k < local_buffers_names.size() ; ++k)
          stream << local_buffers_names[k] << "[lid] = " << accs[k] << ";" << std::endl;


        //Reduce and write final result
        reduce_1d_local_memory(stream, local_size_0_,local_buffers_names,rops);
        for(unsigned int k = 0 ; k < N ; ++k)
          exprs[k]->access_name(local_buffers_names[k]+"[0]");

        stream << "if(lid==0){" << std::endl;
        stream.inc_tab();
        unsigned int i = 0;
        for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it){
          std::string str;
          tree_parsing::traverse(*it, it->root(), tree_parsing::evaluate_expression_traversal(index_tuple("0", "N"), -1, str, mapping[i++]), false);
          stream << str << ";" << std::endl;
        }
        stream.dec_tab();
        stream << "}" << std::endl;
      }

      cl_uint get_vector_size(viennacl::scheduler::statement const & s) const {
        scheduler::statement::container_type exprs = s.array();
        for(scheduler::statement::container_type::iterator it = exprs.begin() ; it != exprs.end() ; ++it){
          if(is_scalar_reduction(*it)){
            scheduler::statement_node const * current_node = &(*it);
            //The LHS of the prod is a vector
            if(current_node->lhs.type_family==scheduler::VECTOR_TYPE_FAMILY)
              return utils::call_on_vector(current_node->lhs, utils::internal_size_fun());
            //The LHS of the prod is a vector expression
            current_node = &exprs[current_node->lhs.node_index];
            if(current_node->lhs.type_family==scheduler::VECTOR_TYPE_FAMILY)
              return utils::call_on_vector(current_node->lhs, utils::internal_size_fun());
            if(current_node->rhs.type_family==scheduler::VECTOR_TYPE_FAMILY)
              return utils::call_on_vector(current_node->lhs, utils::internal_size_fun());
          }
        }
        throw "unexpected expression tree";
      }

      void core(unsigned int kernel_id, utils::kernel_generation_stream& stream, statements_container const & statements, std::vector<mapping_type> const & mapping) const {
        std::vector<mapped_scalar_reduction*> exprs;
        for(std::vector<mapping_type>::const_iterator it = mapping.begin() ; it != mapping.end() ; ++it)
          for(mapping_type::const_iterator iit = it->begin() ; iit != it->end() ; ++iit)
            if(mapped_scalar_reduction * p = dynamic_cast<mapped_scalar_reduction*>(iit->second.get()))
              exprs.push_back(p);

        std::vector<const char *> scalartypes;
        fill_scalartypes(statements, scalartypes);

        if(kernel_id==0){
          core_0(stream,exprs,scalartypes,statements,mapping);
        }
        else{
          core_1(stream,exprs,scalartypes,statements,mapping);
        }
      }

    private:
      unsigned int num_groups_;
      unsigned int decomposition_;
      mutable temporaries_type temporaries_;
    };


  }

}

#endif