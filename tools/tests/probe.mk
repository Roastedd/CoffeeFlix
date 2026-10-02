# make -f desktop.mk -f tools/tests/probe.mk BUILD=<dir> probe-tests
# then: <dir>/probe-test "$(mktemp -d)"   (fixtures are made with ffmpeg; FFMPEG=<path> to choose it)
probe-tests: $(BUILD)/probe-test
$(BUILD)/probe-test: tools/tests/probe_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: probe-tests
