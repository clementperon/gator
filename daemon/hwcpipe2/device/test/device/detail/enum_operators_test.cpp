/*
 * Copyright (c) 2022 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <catch2/catch.hpp>

#include <device/detail/enum_operators.hpp>

namespace test {

enum class test_enum {
    value0,
    value1,
    value2,
};

// HWCPIPE_DEFINE_ENUM_OPERATORS(test_enum);
} // namespace test

SCENARIO("device::detail::define_enum_operators", "[unit]") {
    GIVEN("test::test_enum::value1") {
        auto value = test::test_enum::value1;

        using namespace hwcpipe::device::detail::enum_operators;

        WHEN("to_underlying_type() is called") {
            THEN("The value is converted") { CHECK(to_underlying(value) == 1); }
        }
        WHEN("operator++() is used") {
            THEN("The value is incremented and returned") {
                CHECK(++value == test::test_enum::value2);
                CHECK(value == test::test_enum::value2);
            }
        }
        WHEN("operator++(int) is used") {
            THEN("The value is incremented and previous returned") {
                CHECK(value++ == test::test_enum::value1);
                CHECK(value == test::test_enum::value2);
            }
        }
        WHEN("operator--() is used") {
            THEN("The value is decremented and returned") {
                CHECK(--value == test::test_enum::value0);
                CHECK(value == test::test_enum::value0);
            }
        }
        WHEN("operator--(int) is used") {
            THEN("The value is decremented and previous returned") {
                CHECK(value-- == test::test_enum::value1);
                CHECK(value == test::test_enum::value0);
            }
        }
    }
}
