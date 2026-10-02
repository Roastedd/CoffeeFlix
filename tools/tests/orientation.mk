# make -f desktop.mk -f tools/tests/orientation.mk orientation-tests
# Makes its fixtures with the ffmpeg command (FFMPEG=/path/to/ffmpeg to pick one) in $(BUILD)/orientation-fixtures.
orientation-tests: $(BUILD)/orientation-test
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BUILD)/orientation-test $(BUILD)/orientation-fixtures
$(BUILD)/orientation-test: tools/tests/orientation_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: orientation-tests
