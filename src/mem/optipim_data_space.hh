#ifndef __MEM_OPTIPIM_DATA_SPACE_HH__
#define __MEM_OPTIPIM_DATA_SPACE_HH__

// ---------------------------------------------------------------------------
// Faithful port of OptiPIM's pim_codegen/data_space.h.
//
// A DataSpaceIdx is a 4-D (batch, channel, height, width) coordinate in a
// tensor's data space.  A projection maps loop-iteration variables to those
// four axes; calc_data_space() evaluates the projection for a given loop
// state, and get_hash()/data_space_from_hash() pack/unpack the 4-D index into
// a single integer used for duplicate-row detection.
//
// Identical semantics to the Ramulator original -- only the namespace and
// include guard differ.  No Ramulator types are referenced.
// ---------------------------------------------------------------------------

#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace optipim {

// projection[axis] = list of (loop_var_name, coefficient)
using projection_type = std::vector<std::vector<std::pair<std::string, int>>>;
using loop_state_type = std::map<std::string, int>;

struct DataSpaceIdx {
    int batch;
    int channel;
    int height;
    int width;

    static int calc_proj_id(loop_state_type loop_state,
                            std::vector<std::pair<std::string, int>> projection) {
        int id = 0;
        for (auto p : projection) {
            id += loop_state[p.first] * p.second;
        }
        return id;
    }

    static DataSpaceIdx calc_data_space(loop_state_type loop_state,
                                        projection_type projection) {
        DataSpaceIdx val;
        val.batch   = calc_proj_id(loop_state, projection[0]);
        val.channel = calc_proj_id(loop_state, projection[1]);
        val.height  = calc_proj_id(loop_state, projection[2]);
        val.width   = calc_proj_id(loop_state, projection[3]);
        return val;
    }

    static DataSpaceIdx data_space_from_hash(int hash, DataSpaceIdx bounds) {
        DataSpaceIdx val;
        val.width = hash % bounds.width;
        hash /= bounds.width;
        val.height = hash % bounds.height;
        hash /= bounds.height;
        val.channel = hash % bounds.channel;
        val.batch = hash / bounds.channel;
        return val;
    }

    int get_hash(DataSpaceIdx bounds) {
        int hash = batch;
        hash *= bounds.channel;
        hash += channel;
        hash *= bounds.height;
        hash += height;
        hash *= bounds.width;
        hash += width;
        return hash;
    }

    std::string get_str() {
        std::stringstream stream;
        stream << "[" << batch << "," << channel << "," << height << ","
               << width << "]";
        return stream.str();
    }

    bool operator==(const DataSpaceIdx& a) const {
        return (batch == a.batch && channel == a.channel &&
                height == a.height && width == a.width);
    }
};

} // namespace optipim

#endif // __MEM_OPTIPIM_DATA_SPACE_HH__
