#include <demo/counter.h>
#include <stdexcept>

namespace demo {
static std::size_t live_count = 0;
Counter::Counter() : v_(0), name_("counter") { ++live_count; }
Counter::Counter(int start) : v_(start), name_("counter") { ++live_count; }
Counter::Counter(const Counter& other) : v_(other.v_), name_(other.name_) { ++live_count; }
Counter::~Counter() { --live_count; }
int Counter::value() const noexcept { return v_; }
void Counter::add(int n) {
    if (v_ + n > 100) throw std::out_of_range("the counter would pass 100");
    v_ += n;
}
void Counter::add(int n, int times) { for (int i = 0; i < times; ++i) add(n); }
std::string Counter::name() const { return name_; }
void Counter::set_name(std::string_view n) { name_ = n; }
bool Counter::operator==(const Counter& other) const { return v_ == other.v_; }
double Counter::ratio(double d) const { return v_ / d; }
Counter Counter::doubled() const { Counter c(v_ * 2); c.name_ = name_ + " x2"; return c; }
void Counter::absorb(Counter& other) { v_ += other.v_; other.v_ = 0; }
std::size_t Counter::live() noexcept { return live_count; }
Counter& Counter::self_ref() { return *this; }
}
