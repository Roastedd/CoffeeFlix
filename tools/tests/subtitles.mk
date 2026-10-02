# Subtitle parsing/lookup tests with the real implementation.
# make -f desktop.mk -f tools/tests/subtitles.mk subtitles-tests && build-desktop/subtitles-test
subtitles-tests: $(BUILD)/subtitles-test
$(BUILD)/subtitles-test: tools/tests/subtitles_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: subtitles-tests
