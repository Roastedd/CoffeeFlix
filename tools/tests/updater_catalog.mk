# Build the updater's state-machine tests with the real implementation and an ephemeral test key.
updater-tests: $(BUILD)/updater-catalog-test
$(BUILD)/updater-catalog-test: tools/tests/updater_catalog_test.cpp src/app/updater.cpp $(filter-out $(BUILD)/src/main.o $(BUILD)/src/app/updater.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out src/app/updater.cpp,$^) $(LDLIBS)
.PHONY: updater-tests
