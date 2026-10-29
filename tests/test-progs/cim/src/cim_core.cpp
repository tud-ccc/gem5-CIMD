#include "cim_core.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace std;

namespace cim_core {

namespace {

/** Free rows of one DRAM subarray */
struct SubarrayMeta {
    // Virtual address that maps to this DRAM subarray
    char* virt_addr;
    // Contiguous runs of free rows: start address -> number of rows.
    // Sorted by address, so a freed run can be merged with its neighbours.
    map<char*, size_t> free_runs;

    // newly created subarrays are considered to be fully free
    explicit SubarrayMeta(char* virt_addr):
        virt_addr(virt_addr),
        free_runs{{virt_addr, NR_ROWS_IN_SUBARRAY}}
    {}
};

/** For freeing allocated rows - stored CPU-side */
struct AllocationHeader {
    size_t num_rows;        // Number of rows allocated
    SubarrayMeta* subarray; // Which subarray this belongs to
};

// track available subarrays; heap-allocated so that the pointers below stay
// valid when the vector grows
vector<unique_ptr<SubarrayMeta>> subarrays;
// number of huge pages handed out by `mmapCIM` so far
size_t hugepages_allocated = 0;
using SubarrayLabel = size_t;
unordered_map<SubarrayLabel, SubarrayMeta*> subarray_label_to_subarray;

// CPU-side allocation tracking - no CIM memory wasted
unordered_map<void*, AllocationHeader> alloc_headers;

size_t rows_for(const size_t size)
{
    return (size + BYTES_PER_SUBARRAY_ROW - 1) / BYTES_PER_SUBARRAY_ROW;
}

/** First fit: take `num_rows` rows from the start of the first run that is large enough */
char* take_rows(SubarrayMeta& subarray, const size_t num_rows)
{
    for (auto it = subarray.free_runs.begin(); it != subarray.free_runs.end(); ++it) {
        if (it->second < num_rows)
            continue;
        char* addr = it->first;
        const size_t rows_left = it->second - num_rows;
        subarray.free_runs.erase(it);
        if (rows_left > 0)
            subarray.free_runs.emplace(addr + num_rows * BYTES_PER_SUBARRAY_ROW, rows_left);
        return addr;
    }
    return nullptr;
}

/** Return rows to the subarray, merging with the free runs right before and after */
void give_back_rows(SubarrayMeta& subarray, char* addr, size_t num_rows)
{
    auto& runs = subarray.free_runs;
    auto next = runs.lower_bound(addr);
    if (next != runs.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second * BYTES_PER_SUBARRAY_ROW == addr) {
            addr = prev->first;
            num_rows += prev->second;
            runs.erase(prev);
        }
    }
    if (next != runs.end() && addr + num_rows * BYTES_PER_SUBARRAY_ROW == next->first) {
        num_rows += next->second;
        runs.erase(next);
    }
    runs.emplace(addr, num_rows);
}

} // namespace

void* mmapCIM(void* addr, size_t length)
{
    void* ret = (void*) syscall(__NR_mmapCIM, addr, length);

    if (ret == MAP_FAILED)
        perror("mmapCIM syscall failed");
    return ret;
}

/// Performs allocation of operands in the region reserved for CIM
void* cim_malloc(const size_t size, const size_t subarray_label) {
    // sanity check
    if (size == 0 || size > SUBARRAY_SIZE_BYTES) {
        return nullptr;
    }

    auto it = subarray_label_to_subarray.find(subarray_label);
    if (subarray_label_to_subarray.size() >= NR_SUBARRAYS && it == subarray_label_to_subarray.end()) {
        cerr << "ERROR: out of subarrays (have " << subarray_label_to_subarray.size()
            << " labels, max is " << NR_SUBARRAYS << ")" << endl;
        abort();
    }

    const size_t num_rows = rows_for(size);

    // Helper lambda: allocate from a specific subarray and register header
    auto allocate_from_subarray = [&](SubarrayMeta* subarray) -> void* {
        char* vaddr = take_rows(*subarray, num_rows);
        if (!vaddr) return nullptr;

        subarray_label_to_subarray[subarray_label] = subarray;
        alloc_headers[vaddr] = AllocationHeader{ num_rows, subarray };
        return vaddr;
    };

    // 1. Check if subarray_label refers to an already allocated subarray
    if (it != subarray_label_to_subarray.end()) {
        auto* ptr = allocate_from_subarray(it->second);
        if (!ptr) {
            cerr << "CIM OOM on subarray " << subarray_label << endl;
            return nullptr;
        }
        return ptr;
    }

    // 2. Check if there is some space in already allocated CIM subarrays
    for (auto& subarray : subarrays) {
        auto* ptr = allocate_from_subarray(subarray.get());
        if (ptr) return ptr;
    }

    // 3. Allocate new huge page
    if (hugepages_allocated >= NR_HUGEPAGES) {
        cerr << "ERROR: huge page pool exhausted (" << NR_HUGEPAGES << " huge pages)" << endl;
        return nullptr;
    }
    char* next_hugepage_start = (char*) CIM_BASE_ADDR + hugepages_allocated * HUGE_PAGE_SIZE;
    if (mmapCIM(next_hugepage_start, HUGE_PAGE_SIZE) == MAP_FAILED) {
        std::fprintf(stderr, "cim_malloc: failed to allocate %zu bytes\n", size);
        return nullptr;
    }
    hugepages_allocated++;

    for (size_t i = 0; i < SUBARRAYS_PER_HUGE_PAGE; ++i) {
        subarrays.push_back(make_unique<SubarrayMeta>(next_hugepage_start + SUBARRAY_SIZE_BYTES * i));
    }

    return allocate_from_subarray(subarrays.back().get());
}

/// Frees the memory previously allocated with `cim_malloc()`
void cim_free(void* ptr) {
    if (!ptr) return;

    // Look up allocation header
    auto it = alloc_headers.find(ptr);
    if (it == alloc_headers.end()) {
        cerr << "ERROR: cim_free called with unknown pointer " << ptr << endl;
        return;
    }

    give_back_rows(*it->second.subarray, static_cast<char*>(ptr), it->second.num_rows);
    alloc_headers.erase(it);
}

} // namespace cim_core
