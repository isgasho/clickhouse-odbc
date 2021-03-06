#include "driver/utils/type_info.h"

#include <gtest/gtest.h>

#include <string>
#include <tuple>

class StringPong
    : public ::testing::Test
{
protected:
    template <typename T>
    inline void compare(const std::string & initial_str, const std::string & expected_str, bool case_sensitive = true) {
        T obj;
        value_manip::to_null(obj);
        value_manip::from_value<std::string>::template to_value<T>::convert(initial_str, obj);

        std::string resulting_str;
        value_manip::to_null(resulting_str);
        value_manip::from_value<T>::template to_value<std::string>::convert(obj, resulting_str);

        if (case_sensitive)
            ASSERT_STREQ(resulting_str.c_str(), expected_str.c_str());
        else
            ASSERT_STRCASEEQ(resulting_str.c_str(), expected_str.c_str());
    }
};

template <typename T>
class StringPongSymmetric
    : public StringPong
    , public ::testing::WithParamInterface<std::string>
{
protected:
    using DataType = T;
};

template <typename T>
class StringPongAsymmetric
    : public StringPong
    , public ::testing::WithParamInterface<std::tuple<std::string, std::string>>
{
protected:
    using DataType = T;
};

using StringPongGUIDSymmetric     = StringPongSymmetric<SQLGUID>;
using StringPongNumericSymmetric  = StringPongSymmetric<SQL_NUMERIC_STRUCT>;
using StringPongNumericAsymmetric = StringPongAsymmetric<SQL_NUMERIC_STRUCT>;

TEST_P(StringPongGUIDSymmetric,     Compare) { compare<DataType>(GetParam(), GetParam(), false/* case_sensitive */); }
TEST_P(StringPongNumericSymmetric,  Compare) { compare<DataType>(GetParam(), GetParam()); }
TEST_P(StringPongNumericAsymmetric, Compare) { compare<DataType>(std::get<0>(GetParam()), std::get<1>(GetParam())); }

INSTANTIATE_TEST_SUITE_P(TypeConversion, StringPongGUIDSymmetric,
    ::testing::Values(
        "00000000-0000-0000-0000-000000000000",
        "01020304-0506-0708-090A-0B0C0D0E0F00",
        "10203040-5060-7080-90A0-B0C0D0E0F000",
        "FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"
    )
);

INSTANTIATE_TEST_SUITE_P(TypeConversion, StringPongNumericSymmetric,
    ::testing::Values(
        "0",
        "12345",
        "-12345",
        "12345.6789",
        "-12345.6789",
        "12345.000000000000",
        "12345.001002003000",
        "10000000000000000000",
        "-10000000000000000000",
        ".00000000000000000001",
        "-.0000000000000000001",
        "9876543210987654321",
        ".9876543210987654321",
        "-9876543210987654321",
        "-.9876543210987654321",
        "9999999999999999999",
        "-9999999999999999999",
        ".9999999999999999999",
        "-.9999999999999999999",
        "18446744073709551615",
        "-18446744073709551615",
        ".18446744073709551615",
        "-.18446744073709551615"
    )
);

INSTANTIATE_TEST_SUITE_P(TypeConversion, StringPongNumericAsymmetric,
    ::testing::ValuesIn(std::initializer_list<std::tuple<std::string, std::string>>{
        { "0.", "0" },
        { "-0.", "0" },
        { "0.000", ".000" },
        { "-0.000", ".000" },
        { "0001.00001", "1.00001" },
        { "-0001.00001", "-1.00001" },
        { "000000.123", ".123" }
    })
);
