#ifndef DAGE_RESULT_HPP
#define DAGE_RESULT_HPP

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace dage {

struct Error {
    std::string category;
    std::string code;
    std::string message;
    bool retryable = false;
};

template <typename T, typename E = Error>
class Result {
public:
    static Result success(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    static Result failure(E error) { return Result(std::in_place_index<1>, std::move(error)); }

    bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() {
        if (!has_value()) throw std::logic_error("Result does not contain a value");
        return std::get<0>(storage_);
    }
    const T& value() const {
        if (!has_value()) throw std::logic_error("Result does not contain a value");
        return std::get<0>(storage_);
    }
    E& error() {
        if (has_value()) throw std::logic_error("Result does not contain an error");
        return std::get<1>(storage_);
    }
    const E& error() const {
        if (has_value()) throw std::logic_error("Result does not contain an error");
        return std::get<1>(storage_);
    }

private:
    template <std::size_t I, typename V>
    Result(std::in_place_index_t<I> index, V&& value)
        : storage_(index, std::forward<V>(value)) {}

    std::variant<T, E> storage_;
};

} // namespace dage
#endif
