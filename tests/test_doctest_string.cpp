#include "doctest.h"

#include <string>

TEST_CASE("doctest self append preserves stack and heap strings") {
  for (size_t length : {size_t(0), size_t(3), size_t(20), size_t(40), size_t(100)}) {
    const std::string original(length, 'x');
    doctest::String value(original.c_str());
    value += value;
    CHECK(std::string(value.c_str()) == original + original);
    CHECK(value.size() == length * 2);
  }
}

TEST_CASE("doctest append preserves a distinct source across reallocations") {
  for (size_t length : {size_t(3), size_t(20), size_t(100)}) {
    const std::string original(length, 'a');
    const std::string suffix(100, 'b');
    doctest::String value(original.c_str());
    const doctest::String other(suffix.c_str());
    value += other;
    CHECK(std::string(value.c_str()) == original + suffix);
    CHECK(std::string(other.c_str()) == suffix);
  }
}
