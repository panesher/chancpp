#define BOOST_TEST_MODULE SelectChannelsTest
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>

#include "chan.h"
#include "select.h"
#include <string>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

BOOST_DATA_TEST_CASE(select_basic, boost::unit_test::data::xrange(0, 3)) {
  chan::Chan<int> c_int(sample);
  chan::Chan<float> c_float(sample);
  chan::Chan<std::string> c_string(sample);
  chan::Chan<bool> c_receive_result(5);

  std::atomic<int> expected_idx{0};
  int data_int{1};
  float data_float{1.5};
  std::string data_string{"hello world"};

  std::thread reader([&] {
    while (c_int || c_float || c_string) {
      Select(chan::on(c_int,
                      [&](int v) {
                        BOOST_TEST(expected_idx.load() == 0);
                        BOOST_TEST(data_int == v);
                        c_receive_result << true;
                      }),
             chan::on(c_float,
                      [&](float v) {
                        BOOST_TEST(expected_idx.load() == 1);
                        BOOST_TEST(data_float == v);
                        c_receive_result << true;
                      }),
             chan::on(c_string, [&](std::string v) {
               BOOST_TEST(expected_idx.load() == 2);
               BOOST_TEST(data_string == v);
               c_receive_result << true;
             }));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  expected_idx.store(0);
  c_int << data_int;
  bool has_result;
  has_result << c_receive_result;

  expected_idx.store(1);
  c_float << data_float;
  has_result << c_receive_result;

  expected_idx.store(2);
  c_string << data_string;
  has_result << c_receive_result;

  c_int.close();
  c_float.close();
  c_string.close();

  reader.join();
}
