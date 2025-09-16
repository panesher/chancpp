BOOST_DIR=/opt/homebrew/opt/boost

test:
	clang++ -std=c++20 -O3 -fsanitize=thread -pthread tests/chan_tests.cpp -o build/chan_tests.out \
		-Iinclude -I$(BOOST_DIR)/include -L$(BOOST_DIR)/lib
	./build/chan_tests.out

test_stress:
	clang++ -std=c++20 -O3 -fsanitize=thread -pthread tests/chan_stress_tests.cpp -o build/chan_stress_tests.out \
		-Iinclude -I$(BOOST_DIR)/include -L$(BOOST_DIR)/lib
	./build/chan_stress_tests.out

test_select:
	clang++ -std=c++20 -O3 -fsanitize=thread -pthread tests/select_tests.cpp -o build/select_tests.out \
		-Iinclude -I$(BOOST_DIR)/include -L$(BOOST_DIR)/lib
	./build/select_tests.out

test_all:
		make test
		make test_stress
		make test_select
