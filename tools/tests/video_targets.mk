# Build after tools/build-deps.sh applies the decoder ownership patch.
video-targets-tests: $(BUILD)/video-targets-test
$(BUILD)/video-targets-test: tools/tests/video_targets_test.cpp deps/src/ffmpeg/libavcodec/wiiu_frame_targets.h
	$(CXX) $(CXXFLAGS) -Ideps/src/ffmpeg -o $@ $< $(LDLIBS)
.PHONY: video-targets-tests
