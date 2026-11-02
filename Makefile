CXX ?= c++
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread -MMD -MP
LDFLAGS += -pthread

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))
LIB_OBJECTS := $(filter-out build/main.o,$(OBJECTS))
TEST_SOURCES := $(wildcard tests/test_*.cpp)
TEST_OBJECTS := $(patsubst tests/%.cpp,build/tests/%.o,$(TEST_SOURCES))
TEST_BINS := $(patsubst tests/%.cpp,build/tests/%,$(TEST_SOURCES))

.PHONY: all test clean
all: build/terminal-lyrics

build/terminal-lyrics: $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

build/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/tests/%.o: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/tests/%: build/tests/%.o $(LIB_OBJECTS)
	$(CXX) $^ $(LDFLAGS) $(LDLIBS) -o $@

test: all $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do "$$test"; done
	@sh tests/test_cli.sh "$(CURDIR)/build/terminal-lyrics"

clean:
	$(RM) -r build

.SECONDARY: $(TEST_OBJECTS)
-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
