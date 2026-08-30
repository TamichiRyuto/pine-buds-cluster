# Host-side build for unit tests. Firmware builds live in OpenPineBuds (see docs/).

CXX      := g++
# Kernel sources must stay target-clean: no exceptions/RTTI, no double creep.
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -Wdouble-promotion -Wfloat-conversion \
            -fno-exceptions -fno-rtti -O2 -Isrc

BUILDDIR := build
SRC      := $(wildcard src/*.cpp)
MPISRC   := $(wildcard adapters/mpi/*.cpp)
TESTBIN  := $(BUILDDIR)/test_gemm
MPIBIN   := $(BUILDDIR)/test_mpi_adapter

# OpenPineBuds compiles C++ as gnu++98 with these warning/float settings;
# mirror them on the host so kernel code never drifts off the target dialect.
TARGET_DIALECT := -std=gnu++98 -Wall -Wextra -Werror -Wdouble-promotion \
                  -Wfloat-conversion -fno-exceptions -fno-rtti \
                  -fsingle-precision-constant -Isrc -Ifirmware/pinebuds_compute

.PHONY: test check98 clean

check98:
	@mkdir -p $(BUILDDIR)/check98
	$(CXX) $(TARGET_DIALECT) -Iadapters/mpi -c $(SRC) $(MPISRC) firmware/pinebuds_compute/compute_main.cpp
	@mv *.o $(BUILDDIR)/check98/
	@echo "gnu++98 target-dialect check OK"

test: $(TESTBIN) $(MPIBIN) check98
	./$(TESTBIN)
	./$(MPIBIN)

$(TESTBIN): tests/test_gemm.cpp $(SRC) $(wildcard src/*.h) tests/test_framework.h
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -Itests tests/test_gemm.cpp $(SRC) -o $@

$(MPIBIN): tests/test_mpi_adapter.cpp $(MPISRC) $(wildcard adapters/mpi/*.h) tests/test_framework.h
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -Iadapters/mpi -Itests tests/test_mpi_adapter.cpp $(MPISRC) -o $@

clean:
	rm -rf $(BUILDDIR)
