CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc
LDFLAGS  ?=

BUILD_DIR ?= build
TARGET    := $(BUILD_DIR)/server

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD_DIR)/test_%.o,$(TEST_SRCS))
TEST_DEPS := $(TEST_OBJS:.o=.d)
TEST_TARGETS := $(patsubst tests/%.cpp,$(BUILD_DIR)/%,$(TEST_SRCS))

.PHONY: all test unit-test integration-test edge-integration-test shutdown-test sanitize harden clean
.SECONDARY:

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_%.o: tests/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Test binaries link against every src object except main.o, which owns main().
$(BUILD_DIR)/%: $(BUILD_DIR)/test_%.o $(filter-out $(BUILD_DIR)/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

unit-test: $(TEST_TARGETS)
	@for t in $(TEST_TARGETS); do echo "== $$t"; ./$$t || exit 1; done

integration-test: $(TARGET)
	@python3 tests/integration_test.py $(TARGET)

edge-integration-test: $(TARGET)
	@python3 tests/edge_integration_test.py $(TARGET)

shutdown-test: $(TARGET)
	@python3 tests/shutdown_test.py $(TARGET)

test: unit-test integration-test edge-integration-test shutdown-test

# Memory-safety / undefined-behaviour pass.
#
# Objects land in a separate build directory so sanitizer builds can never be
# linked into the normal build, and that directory is wiped first because make
# does not notice changed CXXFLAGS. -fno-sanitize-recover makes any finding
# abort instead of merely printing and letting the run continue.
#
# SANITIZERS=address,undefined by default. `make sanitize SANITIZERS=undefined`
# runs UBSan alone, which is needed where the ASan runtime cannot initialise.
SANITIZE_DIR   ?= build-sanitize
SANITIZERS     ?= address,undefined
SANITIZE_FLAGS := -fsanitize=$(SANITIZERS) -fno-omit-frame-pointer \
                 -fno-sanitize-recover=all
SANITIZE_CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wpedantic $(SANITIZE_FLAGS)

# libc++ container hardening. This is the available substitute for ASan's
# bounds checking on a toolchain whose ASan runtime cannot start: it inserts
# iterator and index assertions into std::string / std::vector, which is what
# exercises receive-buffer indexing and erase() arithmetic. It changes the
# library ABI, so it gets its own build directory.
HARDEN_DIR        ?= build-harden
HARDEN_CXXFLAGS   := -std=c++17 -O1 -g -Wall -Wextra -Wpedantic \
                     -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG

sanitize:
	@rm -rf $(SANITIZE_DIR)
	$(MAKE) BUILD_DIR=$(SANITIZE_DIR) CXXFLAGS="$(SANITIZE_CXXFLAGS)" test

harden:
	@rm -rf $(HARDEN_DIR)
	$(MAKE) BUILD_DIR=$(HARDEN_DIR) CXXFLAGS="$(HARDEN_CXXFLAGS)" test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(SANITIZE_DIR) $(HARDEN_DIR)

-include $(DEPS) $(TEST_DEPS)
