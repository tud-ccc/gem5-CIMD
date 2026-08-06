#ifndef __MEM_OPTIPIM_LAYOUT_HH__
#define __MEM_OPTIPIM_LAYOUT_HH__

// ---------------------------------------------------------------------------
// Faithful port of OptiPIM's pim_codegen/layout.h.
//
// The Layout class infers, for a tiled loop nest (GEMM or Conv2D), which
// tensor data-space element lands on each (spatial, temporal) slot of the
// PIM substrate.  recursive_infer() emulates the whole loop nest; infer_layout()
// drives it per timestep and builds the per-bank duplicate-detection tables.
//
// Differences from the Ramulator original (semantics unchanged):
//   * The unused IDRAM* member is dropped -- Layout never touches the DRAM
//     organization; only the code generator does.
//   * Ramulator's ImplDef/SpecDef compile-time name tables are replaced by the
//     small runtime NameTable below (same () lookup operators).
//   * ConfigurationError -> std::runtime_error (caught and turned into a gem5
//     panic by the trace player).
// ---------------------------------------------------------------------------

#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "mem/optipim_data_space.hh"

namespace optipim {

// Bidirectional name<->index table replacing Ramulator's ImplDef/SpecDef.
//   table(i)   -> name at index i
//   table(str) -> index of name str (-1 if absent)
struct NameTable {
    std::vector<std::string> names;

    NameTable() {}
    NameTable(std::initializer_list<std::string> l) : names(l) {}

    size_t size() const { return names.size(); }

    std::string operator()(int i) const { return names[i]; }

    int operator()(const std::string& s) const {
        for (size_t i = 0; i < names.size(); i++)
            if (names[i] == s) return (int)i;
        return -1;
    }
};

class Layout {
public:
    std::string m_kernel;
    int n_tensors;
    int m_output_tensor;
    int m_weight_tensor;

    // loop_str: ordered loop names (kernel-specific, e.g. conv2d RSSSPQQCKN)
    std::vector<std::string> m_loops;
    // para_str: parallel tag per loop, 'P' = parallel (spatial), else sequential
    std::vector<char> m_ptags;
    // dim_vec: tile size of each ordered loop
    std::vector<int> m_dims;
    // coeff_vec: address-mapping coefficients per dimension
    std::map<std::string, std::vector<int>> m_coeffs;
    // bank/row_vec: allocation start
    std::vector<int> m_banks;
    std::vector<int> m_rows;
    // levels that split rows spatially/temporally
    int m_row_spatial_level, m_row_temporal_level;
    int m_spatial_banks, m_spatial_cols, m_sequential_steps;

    // Optimization: only emit instructions for one representative bank.
    bool m_single_bank_opt;

    NameTable m_tensors;    // tensor names (Inputs/Filters/Outputs, ...)
    NameTable m_dim_names;  // kernel-specific single-char dimension names

    std::map<int, projection_type> m_projections;
    std::map<int, DataSpaceIdx> m_bounds;

    // 2D matrix of duplicated data per tensor (Y spatial, X temporal),
    // each entry the 4-D data-space index.
    std::vector<std::vector<std::vector<DataSpaceIdx>>> m_pim_tensors;
    // Per-bank per-tensor data layout (hash -> local index) + counts.
    std::vector<std::vector<std::unordered_map<int, int>>> m_bank_tensor_idx;
    std::vector<std::vector<int>> m_bank_tensor_count;
    std::map<int, int> m_pseg_len;

    Layout(std::string kernel, int _n_tensors, int _output_tensors,
           int _filter_tensors, std::vector<std::string> loops,
           std::vector<char> ptags, std::vector<int> dims,
           std::map<std::string, std::vector<int>> coeffs,
           std::vector<int> banks, std::vector<int> rows, bool single_bank_opt)
        : m_kernel(kernel), n_tensors(_n_tensors),
          m_output_tensor(_output_tensors), m_weight_tensor(_filter_tensors),
          m_loops(loops), m_ptags(ptags), m_dims(dims), m_coeffs(coeffs),
          m_banks(banks), m_rows(rows), m_single_bank_opt(single_bank_opt)
    {
        get_spatial_hierarchy();
        if (m_spatial_banks == -1) {
            throw std::runtime_error("OptiPIM layout: illegal layout!");
        }

        for (int i = 0; i < m_spatial_banks; i++) {
            std::vector<std::unordered_map<int, int>> bank_tensor_idx;
            std::vector<int> bank_tensor_count;
            for (int j = 0; j < n_tensors; j++) {
                std::unordered_map<int, int> tensor_idx;
                bank_tensor_idx.push_back(tensor_idx);
                bank_tensor_count.push_back(0);
            }
            m_bank_tensor_idx.push_back(bank_tensor_idx);
            m_bank_tensor_count.push_back(bank_tensor_count);
        }
    }

    virtual ~Layout() {}

    int skip_sequential(size_t& i) {
        int n_sequential_steps = 1;
        for (; i < m_ptags.size(); i++) {
            if (m_ptags[i] == 'P') {
                break;
            } else {
                n_sequential_steps *= m_dims[i];
            }
        }
        return n_sequential_steps;
    }

    int get_advance_spatial(size_t& i) {
        int n_spatial_elems = 1;
        for (; i < m_ptags.size(); i++) {
            if (m_ptags[i] != 'P') {
                break;
            }
            n_spatial_elems *= m_dims[i];
        }
        return n_spatial_elems;
    }

    void get_spatial_hierarchy() {
        bool legal = true;
        size_t i = 0;
        m_spatial_banks = get_advance_spatial(i);
        m_row_temporal_level = i;
        // skip the intermediate (all-sequential) row level
        m_sequential_steps = skip_sequential(i);
        m_row_spatial_level = i;
        m_spatial_cols = get_advance_spatial(i);
        if (i < m_ptags.size()) legal = false;
        if (legal == false) {
            m_spatial_banks = -1;
            m_spatial_cols = -1;
            m_row_temporal_level = -1;
            m_row_spatial_level = -1;
        }
    }

    // Variables for layout inference
    int n_loops, n_spatial_elems;
    int cur_loop, spatial_id, base_spatial;
    loop_state_type loop_state, loop_base, dim_size, dim_cur_level;
    std::vector<int> timestep_index;

    void setup_timestep(int timestep, bool print) {
        if (print) {
            std::cout << "Infer Timestep: " << timestep << std::endl << "\t";
        }
        for (int level = m_row_spatial_level - 1;
             level >= m_row_temporal_level; level--) {
            timestep_index[level] = timestep % m_dims[level];
            timestep /= m_dims[level];
        }
    }

    void recursive_infer(int level) {
        // Only infer one bank under the single-bank optimization.
        if (m_single_bank_opt) {
            if (spatial_id >= m_spatial_cols) return;
        }
        if (level == (int)m_loops.size()) { // last level
            for (int i = 0; i < n_tensors; i++) {
                auto data_space_id =
                    DataSpaceIdx::calc_data_space(loop_state, m_projections[i]);
                m_pim_tensors[i][spatial_id].push_back(data_space_id);
            }
        } else {
            std::string cur_dim = m_loops[level];
            loop_base[cur_dim] *= m_dims[level];

            int remaining_loop = dim_size[cur_dim] / loop_base[cur_dim];
            if (m_coeffs.size() > 0) {
                remaining_loop = 1;
                for (size_t i = dim_cur_level[cur_dim] + 1; i < 3; i++) {
                    remaining_loop *= m_coeffs[cur_dim][i];
                }
            }
            int remaining_elems = 0;

            if (m_ptags[level] == 'P') { // spatially parallel
                base_spatial *= m_dims[level];
                remaining_elems = n_spatial_elems / base_spatial;
            }

            for (int idx = 0; idx < m_dims[level]; idx++) {
                if (timestep_index[level] != -1 &&
                    idx != timestep_index[level]) continue;
                spatial_id += idx * remaining_elems;

                int coeff_delta = idx * remaining_loop;

                dim_cur_level[cur_dim]++;
                loop_state[cur_dim] += coeff_delta;
                recursive_infer(level + 1);
                dim_cur_level[cur_dim]--;
                loop_state[cur_dim] -= coeff_delta;
                spatial_id -= idx * remaining_elems;
            }
            loop_base[cur_dim] /= m_dims[level];
            if (m_ptags[level] == 'P') {
                base_spatial /= m_dims[level];
            }
        }
    }

    void infer_layout(int timestep) {
        bool print = false;
        cur_loop = 0;
        spatial_id = 0;
        n_loops = m_loops.size();
        base_spatial = 1;
        n_spatial_elems = 1;

        for (size_t i = 0; i < m_dim_names.size(); i++) {
            loop_state[std::string(m_dim_names(i))] = 0;
            loop_base[std::string(m_dim_names(i))] = 1;
            dim_size[std::string(m_dim_names(i))] = 1;
            dim_cur_level[std::string(m_dim_names(i))] = 0;
        }
        timestep_index.clear();
        timestep_index.shrink_to_fit();
        for (size_t i = 0; i < m_loops.size(); i++) {
            if (m_ptags[i] == 'P') {
                n_spatial_elems *= m_dims[i];
            }
            dim_size[m_loops[i]] *= m_dims[i];
            timestep_index.push_back(-1);
        }
        setup_timestep(timestep, print);

        m_pim_tensors.clear();
        m_pim_tensors.shrink_to_fit();
        for (int i = 0; i < n_tensors; i++) {
            std::vector<std::vector<DataSpaceIdx>> tensor_dsi(n_spatial_elems);
            m_pim_tensors.push_back(tensor_dsi);
        }

        // Recursively infer the data space by emulating the whole loop.
        recursive_infer(0);

        // Detailed per-bank data layout.
        int temporal_steps = m_pim_tensors[0][0].size();
        for (int tid = 0; tid < temporal_steps; tid++) {
            for (int sid = 0; sid < n_spatial_elems; sid++) {
                int cols_per_bank =
                    int((n_spatial_elems - 1) / m_spatial_banks + 1);
                assert(cols_per_bank == m_spatial_cols);
                int bank_id = int(sid / cols_per_bank);
                if (m_single_bank_opt) {
                    if (bank_id > 0) break;
                }
                for (int i = 0; i < n_tensors; i++) {
                    DataSpaceIdx tensor = m_pim_tensors[i][sid][tid];
                    int tensor_hash = tensor.get_hash(m_bounds[i]);
                    if (m_bank_tensor_idx[bank_id][i].find(tensor_hash) ==
                        m_bank_tensor_idx[bank_id][i].end()) {
                        m_bank_tensor_idx[bank_id][i][tensor_hash] =
                            m_bank_tensor_count[bank_id][i];
                        m_bank_tensor_count[bank_id][i]++;
                    }
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// ConvLayout
// ---------------------------------------------------------------------------
class ConvLayout : public Layout {
public:
    std::vector<int> m_problem_dims;
    int m_Wdialation, m_Hdialation, m_Wstride, m_Hstride;

    ConvLayout(std::vector<int> problem_dims, int Wdilation, int Hdilation,
               int Wstride, int Hstride, std::vector<std::string> loops,
               std::vector<char> ptags, std::vector<int> dims,
               std::map<std::string, std::vector<int>> coeffs,
               std::vector<int> banks, std::vector<int> rows,
               bool single_bank_opt)
        : Layout("conv", 3, 2 /*Outputs*/, 1 /*Filters*/,
                 loops, ptags, dims, coeffs, banks, rows, single_bank_opt)
    {
        m_tensors   = NameTable{"Inputs", "Filters", "Outputs"};
        m_dim_names = NameTable{"N", "K", "P", "Q", "C", "R", "S"};
        //                       Bat, OCh, OH, OW, ICh, FH, FW

        m_problem_dims = problem_dims;
        m_Wdialation = Wdilation;
        m_Hdialation = Hdilation;
        m_Wstride = Wstride;
        m_Hstride = Hstride;

        loop_state_type bound_loop;
        for (size_t i = 0; i < m_dim_names.size(); i++) {
            bound_loop[std::string(m_dim_names(i))] = problem_dims[i];
        }

        // Input projection: (N, C, R*Wdial + P*Wstrd, S*Hdial + Q*Hstrd)
        projection_type iproj = {
            {{"N", 1}},
            {{"C", 1}},
            {{"R", m_Wdialation}, {"P", m_Wstride}},
            {{"S", m_Hdialation}, {"Q", m_Hstride}},
        };
        m_projections[m_tensors("Inputs")] = iproj;
        m_bounds[m_tensors("Inputs")] =
            DataSpaceIdx::calc_data_space(bound_loop, iproj);

        // Filter projection: (C, K, R, S)
        projection_type fproj = {
            {{"C", 1}}, {{"K", 1}}, {{"R", 1}}, {{"S", 1}},
        };
        m_projections[m_tensors("Filters")] = fproj;
        m_bounds[m_tensors("Filters")] =
            DataSpaceIdx::calc_data_space(bound_loop, fproj);

        // Output projection: (N, K, P, Q)
        projection_type oproj = {
            {{"N", 1}}, {{"K", 1}}, {{"P", 1}}, {{"Q", 1}},
        };
        m_projections[m_tensors("Outputs")] = oproj;
        m_bounds[m_tensors("Outputs")] =
            DataSpaceIdx::calc_data_space(bound_loop, oproj);
    }
};

// ---------------------------------------------------------------------------
// GemmLayout
// ---------------------------------------------------------------------------
class GemmLayout : public Layout {
public:
    std::vector<int> m_problem_dims;

    GemmLayout(std::vector<int> problem_dims, std::vector<std::string> loops,
               std::vector<char> ptags, std::vector<int> dims,
               std::map<std::string, std::vector<int>> coeffs,
               std::vector<int> banks, std::vector<int> rows,
               bool single_bank_opt)
        : Layout("gemm", 3, 2 /*Outputs*/, 1 /*Inputs2*/,
                 loops, ptags, dims, coeffs, banks, rows, single_bank_opt)
    {
        m_tensors   = NameTable{"Inputs1", "Inputs2", "Outputs"};
        m_dim_names = NameTable{"N", "H", "P", "Q", "R"};
        //                       Bat, Head/Ch, I1_H, I1_W, I2_W

        m_problem_dims = problem_dims;

        loop_state_type bound_loop;
        for (size_t i = 0; i < m_dim_names.size(); i++) {
            bound_loop[std::string(m_dim_names(i))] = problem_dims[i];
        }

        // Input1 projection: (N, H, P, Q)
        projection_type i1_proj = {
            {{"N", 1}}, {{"H", 1}}, {{"P", 1}}, {{"Q", 1}},
        };
        m_projections[m_tensors("Inputs1")] = i1_proj;
        m_bounds[m_tensors("Inputs1")] =
            DataSpaceIdx::calc_data_space(bound_loop, i1_proj);

        // Input2 projection: (N, H, Q, R)
        projection_type i2_proj = {
            {{"N", 1}}, {{"H", 1}}, {{"Q", 1}}, {{"R", 1}},
        };
        m_projections[m_tensors("Inputs2")] = i2_proj;
        m_bounds[m_tensors("Inputs2")] =
            DataSpaceIdx::calc_data_space(bound_loop, i2_proj);

        // Output projection: (N, H, P, R)
        projection_type oproj = {
            {{"N", 1}}, {{"H", 1}}, {{"P", 1}}, {{"R", 1}},
        };
        m_projections[m_tensors("Outputs")] = oproj;
        m_bounds[m_tensors("Outputs")] =
            DataSpaceIdx::calc_data_space(bound_loop, oproj);
    }
};

} // namespace optipim

#endif // __MEM_OPTIPIM_LAYOUT_HH__
