# make -f desktop.mk -f tools/tests/http_tls.mk http-tls-tests
http-tls-tests: $(BUILD)/http-tls-test
$(BUILD)/http-tls-test: tools/tests/http_tls_test.cpp $(filter-out $(BUILD)/src/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)
.PHONY: http-tls-tests
