# Std lesson 7: Time (`std::time`)

`std::chrono` is powerful and famously hard to read: clocks, durations with ratio templates, `duration_cast`, time points of different clocks that almost convert. `std::time` keeps the one idea that matters (measuring time and telling the date are different things, so they are different types) and drops the rest: one duration type in nanoseconds, two clocks, and civil times and zones for showing dates.

**You will learn:** `std::time::duration` and why it's unsigned, `instant` (the monotonic clock), `wall_time` (the calendar clock), `civil_time`, time zones, `std::thread::sleep`, and testing time-dependent code with a simulated clock.

**You will build:** a rate limiter, frame statistics and time stamps in several zones (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s7 --examples` checks them. `namespace t = std::time;` keeps names short.

---

## 1. Durations

A `duration` is a `u64` count of nanoseconds (enough for about 584 years).

| Make | Read | Arithmetic |
|---|---|---|
| `duration::nanoseconds(n)`, `microseconds`, `milliseconds`, `seconds`, `minutes`, `hours` | `as_nanoseconds()`, `as_microseconds()`, `as_milliseconds()`, `as_seconds()` (rounded down) | `a + b`, `a - b`, `d * n`, `d / n` |
| `duration{}` is zero | `as_seconds_f64()` | `==`, `<`, `<=>` |

- **Every unit is written.** `sleep(5)` can't exist: it's `sleep(t::duration::milliseconds(5))`, which nobody misreads.
- **Never negative.** A duration is a length. `a - b` with `b` longer panics (`a duration can't be negative`), like any unsigned underflow: "earlier minus later" is a bug, not a value.
- `f"{d}"` prints the largest readable unit: `250ns`, `62.5ms`, `2m 15s`.
- The constructors are `compile_time run_time`, so a timeout can be a constant: `compile_time t::duration timeout = t::duration::seconds(30);` at file scope.

Examples: [`durations.ed`](examples/durations.ed), [`err_durations.ed`](examples/err_durations.ed).

## 2. Two clocks, two types

| | `t::instant` | `t::wall_time` |
|---|---|---|
| For | **measuring**: timeouts, frame times, benchmarks | **showing and storing dates** |
| Never goes backwards | yes (monotonic) | no: the user or NTP can change it |
| Meaning of the value | none outside this process | seconds since 1970-01-01 UTC |
| Now | `instant::now()` | `wall_time::now()` |
| Main operations | `i.elapsed()`, `later.since(earlier)`, `i + d`, `i - d` | `utc()`, `local()`, `in_zone(z)`, `unix_nanoseconds()` |

Because they're different types, you can't subtract a calendar time from a monotonic one, which is the bug that makes a C++ timer jump when the clock is adjusted.

```edax
t::instant start = t::instant::now();
do_work();
t::duration took = start.elapsed();
```

`std::thread::sleep(d)` pauses for at least `d`. Timed waits on locks and channels take a `duration` too (std lesson 12).

Example: [`clocks.ed`](examples/clocks.ed).

## 3. Dates: civil time and zones

`wall_time::utc()`, `local()` and `in_zone(z)` give a `civil_time`: `year`, `month` (1–12), `day` (1–31), `hour`, `minute`, `second`, `nanosecond` and `utc_offset_seconds`.

| Zone | Gives |
|---|---|
| `zone::utc()`, `zone::local()` | UTC, and the system's zone |
| `zone::load("Europe/Berlin")` | `std::result<zone, zone_error>`: `not_found` or `invalid_data` |
| `z.name()`, `z.offset_at(t)`, `z.is_dst_at(t)` | |

Zones come from the operating system's database (`/usr/share/zoneinfo`), so daylight-saving rules stay current with system updates. An unknown name is an error value, not a crash.

A `wall_time` is a plain struct (`seconds`, `nanos`), so a fixed moment is `t::wall_time{ .seconds = 1_790_000_000 }`: handy for tests and for timestamps read from files.

Example: [`zones.ed`](examples/zones.ed).

## 4. Testing code that depends on time

`instant` and `wall_time` are values. A function that takes the current instant as a **parameter** instead of calling `now()` inside can be tested with made-up instants, and no test ever sleeps:

```edax
bool allow(t::instant now) mut { ... }

t::instant start = t::instant{};                      // an arbitrary origin
saves.allow(start + t::duration::milliseconds(4999));
```

The real program passes `t::instant::now()`. The exercise is built this way.

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::chrono::milliseconds(5)` | `t::duration::milliseconds(5)` |
| `duration_cast<milliseconds>(d).count()` | `d.as_milliseconds()` |
| `std::chrono::steady_clock::now()` | `t::instant::now()` |
| `std::chrono::system_clock::now()` | `t::wall_time::now()` |
| `std::this_thread::sleep_for(d)` | `std::thread::sleep(d)` |
| `std::chrono::zoned_time{"Europe/Berlin", tp}` | `tp.in_zone(t::zone::load("Europe/Berlin")?)` |
| `std::chrono::year_month_day` | `t::civil_time` |
| negative durations | a panic: durations are lengths |

---

## Your goal

`exercise.ed` has a rate limiter for saves, frame statistics, and time stamps. Fill in the TODOs so that it prints:

```
save at 0ns: saved
save at 1s: skipped
save at 4.99s: skipped
save at 5s: saved
save at 12s: saved
mean frame 20ms, 50 fps
launch 2026-09-21 14:13 UTC
launch 2026-09-21 23:13 in Asia/Tokyo
30 hours later 2026-09-22 20:13 UTC
```

<details>
<summary>Hint 1: <code>allow</code></summary>

`if (last is std::some(previous) && now.since(previous) < interval) { return false; }`, then `last = std::some(now); return true;`. The first call has no `last`, so it's always allowed.
</details>

<details>
<summary>Hint 2: <code>frame_stats</code></summary>

Start from `t::duration{}` (zero) and add; `total / cast<u64>(frames.length())` is the mean. fps: `t::duration::seconds(1).as_nanoseconds() / mean.as_nanoseconds()`.
</details>

<details>
<summary>Hint 3: <code>stamp</code> and 30 hours later</summary>

`t::civil_time c = when.in_zone(zone);`, then `std::format(f"{c.year}-{two(c.month)}-...")`. For later: `launch.seconds + cast::saturate<i64>(t::duration::hours(30).as_seconds())` (`as_seconds` is a `u64`, `seconds` an `i64`).
</details>

---

## Try this afterwards

1. Call `saves.allow(start)` again after the 12 s call.
2. Print `t::duration::milliseconds(3) - t::duration::milliseconds(5)`.
3. Load the zone `"Europe/Berln"`.
4. Measure how long `frame_stats` takes with `t::instant::now()` and `elapsed()`, and print it. Why should that line not be in `expected.txt`?

<details>
<summary>What happens</summary>

1. `panic in rate_limiter::allow: instant::since: 'earlier' is later`. `now` (the start) is before the remembered 12 s instant, which would be a negative duration. Instants from a real clock never go backwards, so this only happens with made-up ones.
2. `panic in main: a duration can't be negative`.
3. `zone::load` returns `std::error(t::zone_error::not_found)`, and the program prints `no Tokyo: not_found` through its `std::error` arm.
4. It prints a few hundred nanoseconds, and a different number on every run: anything measured is not reproducible output.
</details>

## Recap

- One `duration`, unsigned nanoseconds, every unit written; negative is a panic.
- `instant` measures, `wall_time` dates; different types, so they can't be mixed.
- `civil_time` and `zone` turn a `wall_time` into a date in a place.
- Pass "now" as a parameter and time-dependent code becomes testable.

Next: [std lesson 8](../08-files/README.md), files and the environment.
