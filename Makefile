CXX      ?= g++
CXXFLAGS := -std=c++17 -O3 -march=native -Wall -Wextra -pthread
SRCS     := main.cpp disk.cpp workload.cpp measure.cpp simd_multiplier.cpp scalar_multiplier.cpp
OBJS     := $(SRCS:.cpp=.o)

workload: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

# Same kernel source; auto-vectorization is switched off for this one file only.
scalar_multiplier.o: CXXFLAGS += -fno-tree-vectorize -fno-tree-loop-vectorize -fno-tree-slp-vectorize

%.o: %.cpp $(wildcard *.hpp) matmul_kernel.inc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Proof that the two builds really differ: wide-vector instruction counts.
check-simd: simd_multiplier.o scalar_multiplier.o
	@echo "ymm/zmm instructions: simd=$$(objdump -d simd_multiplier.o | grep -cE 'ymm|zmm')  scalar=$$(objdump -d scalar_multiplier.o | grep -cE 'ymm|zmm')"

clean:
	rm -f workload *.o

.PHONY: check-simd clean
