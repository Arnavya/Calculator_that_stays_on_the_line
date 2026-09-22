CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc
LDFLAGS  ?=

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/server

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD_DIR)/test_%.o,$(TEST_SRCS))
TEST_DEPS := $(TEST_OBJS:.o=.d)
TEST_TARGETS := $(patsubst tests/%.cpp,$(BUILD_DIR)/%,$(TEST_SRCS))

.PHONY: all test clean
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

test: $(TEST_TARGETS)
	@for t in $(TEST_TARGETS); do echo "== $$t"; ./$$t || exit 1; done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS) $(TEST_DEPS)
