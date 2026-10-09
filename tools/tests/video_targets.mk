video-targets-tests: $(BUILD)/video-targets-test
$(BUILD)/video-targets-test: tools/tests/video_targets_test.cpp external/FFmpeg-wiiu/libavcodec/wiiu_frame_targets.h
	$(CXX) $(CXXFLAGS) -Iexternal/FFmpeg-wiiu -o $@ $< $(LDLIBS)
.PHONY: video-targets-tests
