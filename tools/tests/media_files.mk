# make -f desktop.mk -f tools/tests/media_files.mk BUILD=<dir> media-files-tests && <dir>/media-files-test
media-files-tests: $(BUILD)/media-files-test
$(BUILD)/media-files-test: tools/tests/media_files_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: media-files-tests
