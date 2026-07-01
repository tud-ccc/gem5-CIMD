#include "cim_core.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gem5/m5ops.h>

using namespace pim_core;

const size_t N_ELEMS = 1000;
size_t next_mat = 0;

template<typename T>
static T* alloc(size_t n_elems) {
	T* p = nullptr;
	do {
		p = static_cast<T*>(cim_malloc(n_elems * sizeof(T), next_mat));
		if (p) break;
		next_mat++;
	} while (next_mat < NR_SUBARRAYS);
	if (!p) {
		std::fprintf(stderr, "ERROR: out of PIM space\n");
		std::exit(1);
	}
	rowtrsp_init(p, n_elems, sizeof(T));
	return p;
}

// Timing is read directly off gem5-CIMD's internal cmd_at advance inside
// executeAmbitMicroprogram() via the RowOp debug flag (see
// "Microprogram DRAM-busy span" DPRINTF in dram_interface.cc), so a single
// invocation per op is enough -- no repeated-calls/slope trick needed.
template<typename T>
static void run_binary_op(const char* name,
		void (*op)(T*, const T*, const T*, size_t, size_t)) {
	T* dst = alloc<T>(N_ELEMS);
	T* src1 = alloc<T>(N_ELEMS);
	T* src2 = alloc<T>(N_ELEMS);

	m5_reset_stats(0, 0);
	op(dst, src1, src2, N_ELEMS, sizeof(T) * 8);
	m5_dump_stats(0, 0);
	std::printf("Ran %s\n", name);
}

template<typename T>
static void run_unary_op(const char* name,
		void (*op)(T*, const T*, size_t, size_t)) {
	T* dst = alloc<T>(N_ELEMS);
	T* src = alloc<T>(N_ELEMS);

	m5_reset_stats(0, 0);
	op(dst, src, N_ELEMS, sizeof(T) * 8);
	m5_dump_stats(0, 0);
	std::printf("Ran %s\n", name);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "all";

	if (!std::strcmp(which, "and32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("and32", rowand<int32_t>);
	if (!std::strcmp(which, "or32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("or32", rowor<int32_t>);
	if (!std::strcmp(which, "xor32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("xor32", rowxor<int32_t>);
	if (!std::strcmp(which, "not32") || !std::strcmp(which, "all"))
		run_unary_op<int32_t>("not32", rownot<int32_t>);
	if (!std::strcmp(which, "add32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("add32", rowadd<int32_t>);
	if (!std::strcmp(which, "sub32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("sub32", rowsub<int32_t>);
	if (!std::strcmp(which, "mul32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("mul32", rowmult<int32_t>);
	if (!std::strcmp(which, "eq32") || !std::strcmp(which, "all"))
		run_binary_op<int32_t>("eq32", rowequal<int32_t>);
	return 0;
}
