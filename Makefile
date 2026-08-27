# Host-side build for unit tests. Firmware builds live in OpenPineBuds (see docs/).

CXX      := g++
# Kernel sources must stay target-clean: no exceptions/RTTI, no double creep.
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -Wdouble-promotion -Wfloat-conversion \
            -fno-exceptions -fno-rtti -O2 -Isrc

BUILDDIR := build
SRC      := $(wildcard src/*.cpp)
TESTBIN  := $(BUILDDIR)/test_gemm

.PHONY: test clean

test: $(TESTBIN)
	./$(TESTBIN)

$(TESTBIN): tests/test_gemm.cpp $(SRC) $(wildcard src/*.h) tests/test_framework.h
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -Itests tests/test_gemm.cpp $(SRC) -o $@

clean:
	rm -rf $(BUILDDIR)
