#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace demo {

class Counter {
public:
    Counter();
    explicit Counter(int start);
    Counter(const Counter& other);
    ~Counter();

    int value() const noexcept;
    void add(int n);                  // throws std::out_of_range when it would pass the limit
    void add(int n, int times);
    std::string name() const;
    void set_name(std::string_view n);
    bool operator==(const Counter& other) const;
    double ratio(double d = 2.0) const;
    Counter doubled() const;
    void absorb(Counter& other);
    static std::size_t live() noexcept;
    Counter& self_ref();              // not wrapped: a reference to a wrapped class

private:
    int         v_;
    std::string name_;
};

template <typename T>
class Box {
public:
    Box() = default;
    T get() const noexcept { return v_; }
    void set(T x) noexcept { v_ = x; }

private:
    T v_{};
};

}
