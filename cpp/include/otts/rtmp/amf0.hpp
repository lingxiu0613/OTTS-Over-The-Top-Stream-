#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace otts::rtmp {

struct Amf0Null {};

struct Amf0Object;

using Amf0Value = std::variant<double, bool, std::string, Amf0Null, Amf0Object>;

struct Amf0Object {
    std::map<std::string, Amf0Value> properties;
};

class Amf0Reader {
public:
    explicit Amf0Reader(const std::vector<std::uint8_t>& data);
    bool read_value(Amf0Value& value);
    bool eof() const;

private:
    bool read_string(std::string& value);
    bool read_object(Amf0Object& object);
    bool read_number(double& value);
    bool read_boolean(bool& value);
    bool can_read(std::size_t size) const;

    const std::vector<std::uint8_t>& data_;
    std::size_t offset_{0};
};

class Amf0Writer {
public:
    void write_string(const std::string& value);
    void write_number(double value);
    void write_boolean(bool value);
    void write_null();
    void write_object(const Amf0Object& object);
    const std::vector<std::uint8_t>& data() const;

private:
    void write_u16(std::uint16_t value);
    void write_u32(std::uint32_t value);

    std::vector<std::uint8_t> data_;
};

std::string as_string(const Amf0Value& value);
double as_number(const Amf0Value& value);

}  // namespace otts::rtmp
