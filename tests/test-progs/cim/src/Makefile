# NOTE: only x86 tested so far
ARCH = x86

# Tools
CXX      := g++
AR       := ar
ARFLAGS  := rcs

# Directories
BIN_DIR   := ../bin
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
LIB_DIR   := $(BUILD_DIR)/lib

# Flags
CXXFLAGS  := -Wall -Wextra -O2 -std=c++20
CXXFLAGS  += -I/home/alex/Documents/Studium/Sem7/Grosser_Beleg_inf_d_950/gem5-CIMD/include
TESTFLAGS := -std=c++20

# Library
LIB       := $(LIB_DIR)/libcim.a
CORE_OBJ  := $(OBJ_DIR)/cim_core.o

# Program Objects
HELLO_WORLD_OBJ:= $(OBJ_DIR)/hello_world.o
FULL_OBJ       := $(OBJ_DIR)/cim_full_program.o
TEST_OBJ       := $(OBJ_DIR)/cim_test_primitives.o
ROWOPS_OBJ     := $(OBJ_DIR)/cim_test_row_ops_8_16.o
CIMMALLOC_OBJ  := $(OBJ_DIR)/cim_test_cimmalloc.o
WORKLOADS_OBJ  := $(OBJ_DIR)/cim_workloads.o
KNN_OBJ        := $(OBJ_DIR)/combined_knn.o
KNN_CPU_OBJ    := $(OBJ_DIR)/combined_knn_cpu.o
KNN_GPU_OBJ    := $(OBJ_DIR)/combined_knn_gpu.o
AXPY_OBJ       := $(OBJ_DIR)/cim_axpy.o
AXPY_CPU_OBJ   := $(OBJ_DIR)/cim_axpy_cpu.o
AXPY_GPU_OBJ   := $(OBJ_DIR)/cim_axpy_gpu.o

# Binaries
HELLO_WORLD_PROG:= $(BIN_DIR)/hello_world
FULL_PROG       := $(BIN_DIR)/cim_full_program
TEST_PROG       := $(BIN_DIR)/cim_test_primitives
ROWOPS_PROG     := $(BIN_DIR)/cim_test_row_ops_8_16
CIMMALLOC_PROG  := $(BIN_DIR)/cim_test_cimmalloc
WORKLOADS_PROG  := $(BIN_DIR)/cim_workloads
KNN_PROG        := $(BIN_DIR)/combined_knn
KNN_CPU_PROG    := $(BIN_DIR)/combined_knn_cpu
KNN_GPU_PROG    := $(BIN_DIR)/combined_knn_gpu
AXPY_PROG       := $(BIN_DIR)/cim_axpy
AXPY_CPU_PROG   := $(BIN_DIR)/cim_axpy_cpu
AXPY_GPU_PROG   := $(BIN_DIR)/cim_axpy_gpu

# Selectable test (TEST=primitives | cimmalloc | workloads | knn | axpy)
TEST ?= all

TEST_helloworld := $(HELLO_WORLD_PROG)
TEST_primitives := $(TEST_PROG)
TEST_rowops8_16 := $(ROWOPS_PROG)
TEST_cimmalloc  := $(CIMMALLOC_PROG)
TEST_workloads  := $(WORKLOADS_PROG)
TEST_knn        := $(KNN_PROG)
TEST_axpy       := $(AXPY_PROG)
TEST_all        := $(HELLO_WORLD_PROG) $(TEST_PROG) $(CIMMALLOC_PROG) $(WORKLOADS_PROG) $(KNN_PROG) $(AXPY_PROG)

VALID_TESTS := all primitives cimmalloc workloads knn axpy rowops8_16
ifeq ($(filter $(TEST),$(VALID_TESTS)),)
$(error Invalid TEST='$(TEST)'. Valid values: $(VALID_TESTS))
endif

# Default targets
all: $(BIN_DIR) $(LIB_DIR) $(OBJ_DIR) $(LIB) $(FULL_PROG) $(KNN_PROG) $(KNN_CPU_PROG)

knn_all: $(BIN_DIR) $(LIB_DIR) $(OBJ_DIR) $(LIB) $(KNN_PROG) $(KNN_CPU_PROG) $(KNN_GPU_PROG)

test: $(BIN_DIR) $(LIB_DIR) $(OBJ_DIR) $(LIB) $(TEST_$(TEST))
	@echo "Built test(s): $(TEST)"

# Create directories
$(BIN_DIR) $(OBJ_DIR) $(LIB_DIR):
	mkdir -p $@

# Object files
$(OBJ_DIR)/hello_world.o: hello_world.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_core.o: cim_core.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_full_program.o: cim_full_program.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_test_primitives.o: cim_test_primitives.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_test_row_ops_8_16.o: cim_test_row_ops_8_16.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_test_cimmalloc.o: cim_test_cimmalloc.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_workloads.o: cim_workloads.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/combined_knn.o: combined_knn.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/combined_knn_cpu.o: combined_knn_cpu.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/combined_knn_gpu.o: combined_knn_gpu.cpp | $(OBJ_DIR)
	hipcc $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_axpy.o: cim_axpy.cpp cim_core.h | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_axpy_cpu.o: cim_axpy_cpu.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(TESTFLAGS) -c $< -o $@

$(OBJ_DIR)/cim_axpy_gpu.o: cim_axpy_gpu.cpp | $(OBJ_DIR)
	hipcc $(TESTFLAGS) -c $< -o $@

# Static library
$(LIB): $(CORE_OBJ) | $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $^

# Link executables
$(HELLO_WORLD_PROG): $(HELLO_WORLD_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -o $@

$(FULL_PROG): $(FULL_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -o $@

$(TEST_PROG): $(TEST_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -L/home/alex/Documents/Studium/Sem7/Grosser_Beleg_inf_d_950/gem5-CIMD/util/m5/build/x86/out -lm5 -o $@

$(ROWOPS_PROG): $(ROWOPS_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -L/home/alex/Documents/Studium/Sem7/Grosser_Beleg_inf_d_950/gem5-CIMD/util/m5/build/x86/out -lm5 -o $@

$(CIMMALLOC_PROG): $(CIMMALLOC_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -o $@

$(WORKLOADS_PROG): $(WORKLOADS_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -o $@

$(KNN_PROG): $(KNN_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -o $@

$(KNN_CPU_PROG): $(KNN_CPU_OBJ) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@

$(KNN_GPU_PROG): $(KNN_GPU_OBJ) | $(BIN_DIR)
	hipcc $< -o $@

$(AXPY_PROG): $(AXPY_OBJ) $(LIB) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L$(LIB_DIR) -lcim -L/home/alex/Documents/Studium/Sem7/Grosser_Beleg_inf_d_950/gem5-CIMD/util/m5/build/x86/out -lm5 -o $@

$(AXPY_CPU_PROG): $(AXPY_CPU_OBJ) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -L/home/alex/Documents/Studium/Sem7/Grosser_Beleg_inf_d_950/gem5-CIMD/util/m5/build/x86/out -lm5 -o $@

$(AXPY_GPU_PROG): $(AXPY_GPU_OBJ) | $(BIN_DIR)
	hipcc $< -o $@

# Cleanup
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN_DIR)/*
