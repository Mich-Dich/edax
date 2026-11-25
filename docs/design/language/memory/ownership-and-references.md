# Ownership, Copies, Moves and References

## Rules

### Copy types vs. owning types
- **Copy types** are copied implicitly by assignment and by-value passing. These are: all built-in types, `std::raw_ptr`, enums whose payloads are copy types, and structs/classes whose fields are all copy types and that have no destructor.
- **Every other type** (anything owning memory or a resource, e.g. `std::string`, `std::darray`, `std::unique_ptr`, `std::file`) is **never copied implicitly**. You either:
  - `move x` to transfer ownership; `x` can't be used afterwards (checked at compile time), or
  - `x.clone()` to make a visible copy, which usually allocates.

```edax
std::string a = std::string::from("hi");
std::string b = a;                // ERROR: std::string is not a copy type
std::string c = a.clone();        // OK: visible allocation
std::string d = move a;           // OK: a is now unusable
std::println(a);                  // ERROR: a was moved
```

### Where moves are implicit
1. **Temporaries** are always moved: `push(employee{...})`, `std::ok(parse())`.
2. **`return x;`** of a local variable or by-value parameter.

Everything else needs `move`.

### Moving out of fields
`move` works on local variables and by-value parameters only. To take a field's value, use `std::take(mut obj.field)`, which leaves the field's default behind, or `std::replace(mut obj.field, new_value)`.

### Conditional moves
If a variable is moved in only some branches, it counts as moved after them. It can be used again only after a new assignment, which requires it to be `mut`.

### Destruction (RAII)
- Locals are destroyed at the end of their scope, in **reverse declaration order**.
- A moved-from variable is not destroyed.
- Temporaries are destroyed at the end of the full statement.
- An object's destructor body runs first, then its fields are destroyed in reverse declaration order.

### References
`T&` (immutable) and `mut T&` (mutable) never own anything, and they are **second-class**: they can't outlive what they point to, and this is enforced *without* lifetime annotations.

| References may appear as | References may **not** appear as |
|---|---|
| Function and method parameters | Struct/class fields (except in `view` types) |
| `for` loop variables, `match` bindings | Return types (except by the rule below) |
| Local variables (`employee& e = list[0];`) | Elements stored in non-`view` containers |
| Lambda captures (the lambda becomes a view) | |

**Returning a reference:** allowed only if it points into `this` (for methods) or into the **only** reference/view parameter. The compiler checks this inside the function body.
```edax
std::str trim(std::str s) { ... }                    // OK: borrows from s
T& operator[](usize i) { ... }                       // OK: borrows from this
std::str pick(std::str a, std::str b) { ... }        // ERROR: ambiguous source
```

### Aliasing rule
Within one function the compiler checks that, while a reference is alive:

- **a `mut T&` is the only way to reach that object**, and
- **a `T&` means the object is not changed**.

```edax
for (employee& e : employees) {
    employees.push(e.clone());            // ERROR: employees is borrowed by the loop
}
swap(mut a, mut a);                       // ERROR: two mutable references to a
```

### Places: fields are separate
The aliasing rule is about **places**. A place is a local (or `this`) followed by fields: `w`, `w.positions`, `w.camera.position`. Two places of one variable overlap when one is a prefix of the other (`w` and `w.positions`); places that part ways at a field (`w.positions` and `w.velocities`) don't. An index ends a place: `xs[i]` and `xs[j]` are the same place, because `i` may equal `j`.
```edax
integrate(mut w.positions, w.velocities);   // OK: two fields of w
w.positions.push(w.velocities[0]);          // OK
w.positions.push(w.positions[0]);           // OK: the value is copied before push starts
integrate(mut w.positions, w.positions);    // ERROR: w.positions is borrowed mutably and shared
swap(mut xs[i], mut xs[j]);                 // ERROR: one place; use xs.swap(i, j)
```
- **Value arguments are read first (two-phase).** An argument that binds to a value parameter (not a reference, and not a `view` type) is copied before the call starts, so it may read the place that the call's receiver or a `mut` argument borrows: `v.push(v[0])`, `ctx.state_of(ctx.open_menu)`, `bump(mut c, c.hits)`. A reference argument still may not overlap a mutable one.
- Inside one function, the borrow of a place is tracked by its fields: a reference parameter's fields (`mut w.a, w.b` with `mut world& w`), local references to fields, and loops (`for (f32& v : w.b) { w.a.push(v); }` borrows only `w.b`). Across a call, a `mut` method borrows all of `this`; to hand out several fields at once, return them together (below).
- Containers hand out several elements with methods that check at run time: `split_at`, `chunks`, `get_many_mut([a, b])` (none when two handles are equal).

### Returning several references
A method may return a **tuple of references into distinct fields of `this`** (the checker verifies that the fields differ):
```edax
class physics_world {
public:
    (mut body_store&, contact_list&) bodies_and_contacts() mut {
        return (mut bodies, contacts);
    }
}
(mut body_store& bodies, contact_list& contacts) = world.bodies_and_contacts();
```
- The elements must be places in `this` or in reference parameters (never locals), and a `mut` element must not overlap any other element.
- The bindings borrow the object like a returned reference does: while `bodies` lives, `world` is borrowed mutably.

### Moving is copying bytes
No Edax value can point into itself: references aren't stored and views can't outlive their source. So **every move is a copy of the bytes**, followed by forgetting the source. Containers move their elements with `memcpy`, whatever the element type (see [Performance](../performance/performance.md)).

### `view` types
A `view` type holds references or pointers into memory it doesn't own. `std::str`, `std::span<T>`, `std::format_args` and iterators are view types. View types follow **the same rules as references**. Both `view struct` and `view class` exist.
```edax
view struct str {
    std::raw_ptr<u8> data;
    usize            length;
}
```
- A type with a field of a view type must itself be declared `view`.
- A template instantiated with a reference or view argument is a view type: `std::darray<std::str>` and `std::option<employee&>` can be locals but can't be returned (except by the rule above) or stored in fields.
- `std::static_str` (string literals) is **not** a view. It points to static memory and can be stored anywhere.
- **A view passed into a view keeps its borrow:** a function returning a view that has exactly one view parameter returns something that borrows what that parameter borrows. A view passed to a `mut` method of a view object (`parts.push(word)`) borrows for as long as that object is used. This is how a borrowing thread keeps its captures borrowed until it is joined (see [Threads](../../stdlib/concurrency/threads.md)).

## Rationale
- **Nothing costs more than it looks:** copying an `employee` would silently allocate a new `std::string`. So owning types are never copied implicitly: every allocation is a visible `.clone()` or a constructor/factory call.
- **Second-class references:** most real lifetime needs are "borrow for the duration of this call or loop". Restricting references to those places gives memory safety with no lifetime syntax. There are no lifetime annotations: an API that would need one (a result that borrows from one of two inputs) returns an owned value or is split into two functions.
- **Local-only aliasing check:** it is per-function, so it stays fast and its error messages stay local. That keeps principle 9.
- **Places instead of variables:** engine code passes one component array mutably and another immutably all the time. Tracking fields keeps that legal with no extra syntax, while two indexes into one container stay one place, because whether they differ is only known at run time.
- **The optimizer uses the rule:** because `mut T&` is checked to be the only way to the object, the compiler marks it `noalias` for LLVM (see [Performance](../performance/performance.md)). The safety rule is also a speed rule.
