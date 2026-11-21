# Time: `std::time`

Two clocks and one duration type. Durations are never negative and never implicit: every unit is written.

## Rules

### `std::time::duration`
A span of time, stored as a `u64` count of nanoseconds (about 584 years).

| Call | Result |
|---|---|
| `duration::nanoseconds(n)`, `microseconds(n)`, `milliseconds(n)`, `seconds(n)`, `minutes(n)`, `hours(n)` | A duration (`n` is a `u64`; overflow panics). `compile_time run_time` |
| `d.as_nanoseconds()`, `as_microseconds()`, `as_milliseconds()`, `as_seconds()` | `u64`, rounded down |
| `d.as_seconds_f64()` | `f64` |
| `a + b`, `a - b`, `d * n`, `d / n` | Arithmetic; a negative result or overflow panics |
| `==`, `<=>` | Comparison |

Formatted with `f"{d}"` in the largest unit that keeps it readable: `250ns`, `1.5ms`, `2.25s`, `3m 12s`.

### `std::time::instant`: the monotonic clock
For measuring time inside the program. It never goes backwards and is unrelated to the calendar.

| Call | Result |
|---|---|
| `instant::now()` | The current instant |
| `i.elapsed()` | `duration` since `i` |
| `later.since(earlier)` | `duration` between two instants; panics if `earlier` is later |
| `i + d`, `i - d` | Another instant |

### `std::time::wall_time`: the calendar clock
For showing and storing dates. It can jump (the user or NTP changes it).

| Call | Result |
|---|---|
| `wall_time::now()` | The current time |
| `t.unix_nanoseconds()` | `i64` nanoseconds since 1970-01-01 UTC |
| `t.utc()`, `t.local()` | A `std::time::civil_time` |
| `t.in_zone(z)` | A `std::time::civil_time` in the time zone `z` |

```edax
struct civil_time {                // std::time::civil_time
    i32 year;
    u8  month;                     // 1..=12
    u8  day;                       // 1..=31
    u8  hour;
    u8  minute;
    u8  second;
    u32 nanosecond;
    i32 utc_offset_seconds;        // 0 for utc(), the system's offset for local()
}
```

### Time zones: `std::time::zone`
| Call | Result |
|---|---|
| `zone::utc()`, `zone::local()` | The UTC zone and the system's zone |
| `zone::load(name) → std::result<std::time::zone, std::time::zone_error>` | A zone from the operating system's time-zone database by IANA name (`"Europe/Oslo"`) |
| `z.name()` | The zone's name |

- Zones come from the OS's database (`/usr/share/zoneinfo` and the like), so they follow the system's updates. A target without the `files` capability has only `utc()` and `local()`.
- `zone_error` is `not_found` or `invalid_data`.

### Waiting
`std::thread::sleep(d)` pauses the current thread for at least `d` (see [Threads](../concurrency/threads.md)). Timed waits on locks and channels take a `duration` too.

## Rationale
- **Two clocks, two types:** measuring with a calendar clock breaks when the clock is changed, and dates from a monotonic clock mean nothing. Separate types make mixing them a compile error.
- **Units are always written:** `sleep(5)` can't exist; `sleep(std::time::duration::milliseconds(5))` can't be misread.
- **Unsigned durations:** a duration is a length. "Earlier minus later" is a bug, so it panics like any underflow.
