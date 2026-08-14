#include "otts/rtmp/amf0.hpp"

#include <cstring>

namespace otts::rtmp {

Amf0Reader::Amf0Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

bool Amf0Reader::read_value(Amf0Value& value) {
    if (!can_read(1)) {
        return false;
    }

    const auto marker = data_[offset_++];
    switch (marker) {
        case 0x00: {
            double number = 0.0;
            if (!read_number(number)) {
                return false;
            }
            value = number;
            return true;
        }
        case 0x01: {
            bool boolean_value = false;
            if (!read_boolean(boolean_value)) {
                return false;
            }
            value = boolean_value;
            return true;
        }
        case 0x02: {
            std::string string_value;
            if (!read_string(string_value)) {
                return false;
            }
            value = string_value;
            return true;
        }
        case 0x03: {
            Amf0Object object;
            if (!read_object(object)) {
                return false;
            }
            value = object;
            return true;
        }
        case 0x05:
            value = Amf0Null{};
            return true;
        default:
            return false;
    }
}

bool Amf0Reader::eof() const {
    return offset_ >= data_.size();
}

bool Amf0Reader::read_string(std::string& value) {
    if (!can_read(2)) {
        return false;
    }

    const auto length = static_cast<std::uint16_t>((data_[offset_] << 8) | data_[offset_ + 1]);
    offset_ += 2;

    if (!can_read(length)) {
        return false;
    }

    value.assign(reinterpret_cast<const char*>(&data_[offset_]), length);
    offset_ += length;
    return true;
}

bool Amf0Reader::read_object(Amf0Object& object) {
    while (true) {
        if (!can_read(3)) {
            return false;
        }

        const auto key_length = static_cast<std::uint16_t>((data_[offset_] << 8) | data_[offset_ + 1]);
        offset_ += 2;

        if (key_length == 0 && data_[offset_] == 0x09) {
            ++offset_;
            return true;
        }

        if (!can_read(key_length + 1)) {
            return false;
        }

        std::string key(reinterpret_cast<const char*>(&data_[offset_]), key_length);
        offset_ += key_length;

        Amf0Value value;
        if (!read_value(value)) {
            return false;
        }
        object.properties.emplace(std::move(key), std::move(value));
    }
}

bool Amf0Reader::read_number(double& value) {
    if (!can_read(8)) {
        return false;
    }

    std::uint8_t bytes[8];
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    for (int i = 0; i < 8; ++i) {
        bytes[7 - i] = data_[offset_ + i];
    }
#else
    for (int i = 0; i < 8; ++i) {
        bytes[i] = data_[offset_ + i];
    }
#endif
    offset_ += 8;
    std::memcpy(&value, bytes, sizeof(value));
    return true;
}

bool Amf0Reader::read_boolean(bool& value) {
    if (!can_read(1)) {
        return false;
    }

    value = data_[offset_++] != 0;
    return true;
}

bool Amf0Reader::can_read(std::size_t size) const {
    return offset_ + size <= data_.size();
}

void Amf0Writer::write_string(const std::string& value) {
    data_.push_back(0x02);
    write_u16(static_cast<std::uint16_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
}

void Amf0Writer::write_number(double value) {
    data_.push_back(0x00);

    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    std::uint8_t bytes[8];
    std::memcpy(bytes, &raw, sizeof(bytes));
    for (int i = 7; i >= 0; --i) {
        data_.push_back(bytes[i]);
    }
#else
    for (int i = 7; i >= 0; --i) {
        data_.push_back(static_cast<std::uint8_t>((raw >> (i * 8)) & 0xFF));
    }
#endif
}

void Amf0Writer::write_boolean(bool value) {
    data_.push_back(0x01);
    data_.push_back(value ? 1 : 0);
}

void Amf0Writer::write_null() {
    data_.push_back(0x05);
}

void Amf0Writer::write_object(const Amf0Object& object) {
    data_.push_back(0x03);
    for (const auto& [key, value] : object.properties) {
        write_u16(static_cast<std::uint16_t>(key.size()));
        data_.insert(data_.end(), key.begin(), key.end());

        std::visit(
            [this](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, double>) {
                    write_number(item);
                } else if constexpr (std::is_same_v<T, bool>) {
                    write_boolean(item);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    write_string(item);
                } else if constexpr (std::is_same_v<T, Amf0Null>) {
                    write_null();
                } else if constexpr (std::is_same_v<T, Amf0Object>) {
                    write_object(item);
                }
            },
            value);
    }

    data_.push_back(0x00);
    data_.push_back(0x00);
    data_.push_back(0x09);
}

const std::vector<std::uint8_t>& Amf0Writer::data() const {
    return data_;
}

void Amf0Writer::write_u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data_.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void Amf0Writer::write_u32(std::uint32_t value) {
    data_.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    data_.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data_.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::string as_string(const Amf0Value& value) {
    if (const auto* result = std::get_if<std::string>(&value)) {
        return *result;
    }
    return {};
}

double as_number(const Amf0Value& value) {
    if (const auto* result = std::get_if<double>(&value)) {
        return *result;
    }
    return 0.0;
}

}  // namespace otts::rtmp
