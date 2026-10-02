# make -f desktop.mk -f tools/tests/images.mk images-tests && build-desktop/images-test
images-tests: $(BUILD)/images-test
$(BUILD)/images-test: tools/tests/images_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: images-tests
