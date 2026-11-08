CXX      ?= clang++
BUILD    ?= build
CORE     ?= src/flight_recorder.cpp
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Itests -Istrategies
ifdef SAN
CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

HEADERS := include/fr/flight_recorder.h tests/feed.h tests/strategies.h strategies/real_ravi.h

.PHONY: all test test-small-ring test-sync-failure test-crc-match check bench bench-long clean
all: $(BUILD)/fr_tool $(BUILD)/run_tests $(BUILD)/ravi_tests $(BUILD)/bench_record $(BUILD)/postmortem

$(BUILD)/core.o: $(CORE) include/fr/flight_recorder.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $(CORE) -o $@

$(BUILD)/fr_tool: tests/fr_tool.cpp $(BUILD)/core.o $(HEADERS)
	$(CXX) $(CXXFLAGS) tests/fr_tool.cpp $(BUILD)/core.o -o $@

$(BUILD)/run_tests: tests/run_tests.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tests/run_tests.cpp -o $@

$(BUILD)/bench_record: tests/bench_record.cpp $(BUILD)/core.o $(HEADERS)
	$(CXX) $(CXXFLAGS) tests/bench_record.cpp $(BUILD)/core.o -o $@

$(BUILD)/postmortem: tools/postmortem.cpp $(BUILD)/core.o $(HEADERS)
	$(CXX) $(CXXFLAGS) tools/postmortem.cpp $(BUILD)/core.o -o $@

$(BUILD)/ravi_tests: tests/ravi_tests.cpp $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tests/ravi_tests.cpp -o $@

test: $(BUILD)/fr_tool $(BUILD)/run_tests $(BUILD)/ravi_tests
	$(BUILD)/ravi_tests
	$(BUILD)/run_tests $(BUILD)

# A tiny ring fills constantly, so this proves the full-ring wait really works.
test-small-ring:
	$(MAKE) test BUILD=$(BUILD)/small_ring CXXFLAGS="$(CXXFLAGS) -DFR_RING_BYTES=4096"

# A failed disk sync can't be caused on demand, so this build fakes one.
test-sync-failure:
	$(MAKE) $(BUILD)/sync_fail/fr_tool BUILD=$(BUILD)/sync_fail CXXFLAGS="$(CXXFLAGS) -DFR_TEST_SYNC_FAILS"
	@rm -f $(BUILD)/sync_fail/log
	@$(BUILD)/sync_fail/fr_tool record clean $(BUILD)/sync_fail/log 1000 0 >/dev/null 2>&1; \
	 test $$? -eq 2 && echo "PASS failed_disk_sync_fails_loudly" || { echo "FAIL failed_disk_sync_fails_loudly"; exit 1; }

# The hardware and software checksums must read each other's logs.
test-crc-match: $(BUILD)/fr_tool
	$(MAKE) $(BUILD)/soft_crc/fr_tool BUILD=$(BUILD)/soft_crc CXXFLAGS="$(CXXFLAGS) -DFR_SOFT_CRC"
	@rm -f $(BUILD)/crc.log
	@$(BUILD)/fr_tool record clean $(BUILD)/crc.log 1000 0 >/dev/null
	@$(BUILD)/soft_crc/fr_tool replay clean $(BUILD)/crc.log 0 | grep -q status=match \
	 && echo "PASS hardware_and_software_crc_agree" || { echo "FAIL hardware_and_software_crc_agree"; exit 1; }

check: test test-small-ring test-sync-failure test-crc-match

bench: $(BUILD)/bench_record
	$(BUILD)/bench_record $(BUILD)

# Long enough for disk syncs to happen during the run. Writes about 1.2 GB a round.
bench-long: $(BUILD)/bench_record
	$(BUILD)/bench_record $(BUILD) 20000000 3

clean:
	rm -rf $(BUILD)
