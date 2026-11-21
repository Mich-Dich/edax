// The Rift runtime, written as LLVM IR (M8). It replaces the C runtime: output buffers, f-string
// formatting (spec 19), string output, panics (spec 11) and main's exit status (spec 04). Every
// generated module is linked with it, so a program needs no C compiler.
//
// Layouts (all as in the generated code):
//   rift_str   { ptr, i64 }              bytes and length of a string
//   rift_buf   { ptr, i64, i64 }         data, length, capacity of an output buffer
//   rift_spec  { i8, i1, i32, i32, i8 }  align, plus, width (-1: none), precision (-1: none), radix
//   rift_loc   { ptr, i32, i32 }         file, line, column of a source location

#include "rift/llvm_backend.hpp"

#include <string>

namespace rift {
namespace {

// A NUL-terminated private constant `@name` holding `text`.
std::string cstring_global(const std::string& name, const std::string& text) {
    std::string out = "@" + name + " = private constant [" + std::to_string(text.size() + 1) + " x i8] c\"";
    for (char c : text) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f && c != '"' && c != '\\') {
            out += c;
        } else {
            static const char* hex = "0123456789ABCDEF";
            out += '\\';
            out += hex[u >> 4];
            out += hex[u & 15];
        }
    }
    return out + "\\00\"\n";
}

// A constant of exactly `text.size()` bytes (no terminator), for lengths passed explicitly.
std::string bytes_global(const std::string& name, const std::string& text) {
    std::string out = "@" + name + " = private constant [" + std::to_string(text.size()) + " x i8] c\"";
    for (char c : text) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f && c != '"' && c != '\\') {
            out += c;
        } else {
            static const char* hex = "0123456789ABCDEF";
            out += '\\';
            out += hex[u >> 4];
            out += hex[u & 15];
        }
    }
    return out + "\"\n";
}

const char* kTypesAndDeclarations = R"IR(
%rift_str = type { ptr, i64 }
%rift_buf = type { ptr, i64, i64 }
%rift_spec = type { i8, i1, i32, i32, i8 }
%rift_loc = type { ptr, i32, i32 }

@stdout = external global ptr
@stderr = external global ptr

declare ptr @malloc(i64)
declare ptr @realloc(ptr, i64)
declare void @free(ptr)
declare void @abort()
declare i64 @fwrite(ptr, i64, i64, ptr)
declare i32 @fputc(i32, ptr)
declare i32 @fflush(ptr)
declare void @flockfile(ptr)
declare void @funlockfile(ptr)
declare i64 @syscall(i64, ...)
declare i32 @memcmp(ptr, ptr, i64)
declare i64 @strlen(ptr)
declare void @exit(i32) noreturn
declare i32 @snprintf(ptr, i64, ptr, ...)
declare double @strtod(ptr, ptr)
declare double @llvm.fabs.f64(double)
declare double @llvm.floor.f64(double)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

@rift_plain_spec = private constant %rift_spec { i8 0, i1 false, i32 -1, i32 -1, i8 0 }
@rift_digits_lower = private constant [16 x i8] c"0123456789abcdef"
@rift_digits_upper = private constant [16 x i8] c"0123456789ABCDEF"
)IR";

// Buffers and padding -------------------------------------------------------------------------

const char* kBuffers = R"IR(
define internal void @rift_buf_put(ptr %b, ptr %s, i64 %n) {
entry:
  %datap = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 0
  %lenp = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 1
  %capp = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 2
  %len = load i64, ptr %lenp
  %cap = load i64, ptr %capp
  %need = add i64 %len, %n
  %grow = icmp ugt i64 %need, %cap
  br i1 %grow, label %resize, label %copy
resize:
  %dbl = mul i64 %cap, 2
  %small = icmp ult i64 %dbl, 128
  %floor = select i1 %small, i64 128, i64 %dbl
  %tooSmall = icmp ult i64 %floor, %need
  %newcap = select i1 %tooSmall, i64 %need, i64 %floor
  %old = load ptr, ptr %datap
  %new = call ptr @realloc(ptr %old, i64 %newcap)
  %failed = icmp eq ptr %new, null
  br i1 %failed, label %oom, label %store
oom:
  %err = load ptr, ptr @stderr
  call i64 @fwrite(ptr @rift_oom, i64 20, i64 1, ptr %err)
  call void @abort()
  unreachable
store:
  store ptr %new, ptr %datap
  store i64 %newcap, ptr %capp
  br label %copy
copy:
  %data = load ptr, ptr %datap
  %dst = getelementptr i8, ptr %data, i64 %len
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %s, i64 %n, i1 false)
  store i64 %need, ptr %lenp
  ret void
}

define internal void @rift_put_repeat(ptr %b, i8 %c, i64 %n) {
entry:
  %cell = alloca i8
  %i = alloca i64
  store i8 %c, ptr %cell
  store i64 0, ptr %i
  br label %cond
cond:
  %iv = load i64, ptr %i
  %more = icmp ult i64 %iv, %n
  br i1 %more, label %body, label %done
body:
  call void @rift_buf_put(ptr %b, ptr %cell, i64 1)
  %next = add i64 %iv, 1
  store i64 %next, ptr %i
  br label %cond
done:
  ret void
}

define internal i64 @rift_count_chars(ptr %s, i64 %n) {
entry:
  %i = alloca i64
  %count = alloca i64
  store i64 0, ptr %i
  store i64 0, ptr %count
  br label %cond
cond:
  %iv = load i64, ptr %i
  %more = icmp ult i64 %iv, %n
  br i1 %more, label %body, label %done
body:
  %p = getelementptr i8, ptr %s, i64 %iv
  %byte = load i8, ptr %p
  %top = and i8 %byte, -64
  %cont = icmp eq i8 %top, -128
  %inc = select i1 %cont, i64 0, i64 1
  %cv = load i64, ptr %count
  %cv2 = add i64 %cv, %inc
  store i64 %cv2, ptr %count
  %next = add i64 %iv, 1
  store i64 %next, ptr %i
  br label %cond
done:
  %r = load i64, ptr %count
  ret i64 %r
}

; Writes `s` padded to the spec's width; `def` is the default alignment ('<' or '>').
define internal void @rift_pad(ptr %b, ptr %s, i64 %n, ptr %spec, i8 %def) {
entry:
  %chars = call i64 @rift_count_chars(ptr %s, i64 %n)
  %wp = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 2
  %w32 = load i32, ptr %wp
  %wneg = icmp slt i32 %w32, 0
  %w32z = select i1 %wneg, i32 0, i32 %w32
  %width = zext i32 %w32z to i64
  %bigger = icmp ugt i64 %width, %chars
  %diff = sub i64 %width, %chars
  %fill = select i1 %bigger, i64 %diff, i64 0
  %ap = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 0
  %al = load i8, ptr %ap
  %none = icmp eq i8 %al, 0
  %a = select i1 %none, i8 %def, i8 %al
  %isr = icmp eq i8 %a, 62
  %isc = icmp eq i8 %a, 94
  %half = udiv i64 %fill, 2
  %lc = select i1 %isc, i64 %half, i64 0
  %left = select i1 %isr, i64 %fill, i64 %lc
  %right = sub i64 %fill, %left
  call void @rift_put_repeat(ptr %b, i8 32, i64 %left)
  call void @rift_buf_put(ptr %b, ptr %s, i64 %n)
  call void @rift_put_repeat(ptr %b, i8 32, i64 %right)
  ret void
}

define internal void @rift_fmt_str(ptr %b, ptr %s, i64 %n, ptr %spec) {
entry:
  call void @rift_pad(ptr %b, ptr %s, i64 %n, ptr %spec, i8 60)
  ret void
}

define internal void @rift_fmt_bool(ptr %b, i1 %v, ptr %spec) {
entry:
  %s = select i1 %v, ptr @rift_true, ptr @rift_false
  %n = select i1 %v, i64 4, i64 5
  call void @rift_pad(ptr %b, ptr %s, i64 %n, ptr %spec, i8 60)
  ret void
}

; Integers: the magnitude in `v`, with a '-' when `neg`, a '+' with the plus flag, in the radix.
define internal void @rift_fmt_u(ptr %b, i128 %v, i1 %neg, ptr %spec) {
entry:
  %tmp = alloca [160 x i8]
  %vv = alloca i128
  %idx = alloca i64
  %rp = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 4
  %r = load i8, ptr %rp
  %isx = icmp eq i8 %r, 120
  %isX = icmp eq i8 %r, 88
  %isb = icmp eq i8 %r, 98
  %iso = icmp eq i8 %r, 111
  %hex = or i1 %isx, %isX
  %b1 = select i1 %isb, i128 2, i128 10
  %b2 = select i1 %iso, i128 8, i128 %b1
  %base = select i1 %hex, i128 16, i128 %b2
  %digits = select i1 %isX, ptr @rift_digits_upper, ptr @rift_digits_lower
  store i128 %v, ptr %vv
  store i64 160, ptr %idx
  br label %loop
loop:
  %cur = load i128, ptr %vv
  %d = urem i128 %cur, %base
  %q = udiv i128 %cur, %base
  %di = trunc i128 %d to i64
  %dp = getelementptr inbounds [16 x i8], ptr %digits, i64 0, i64 %di
  %ch = load i8, ptr %dp
  %i0 = load i64, ptr %idx
  %i1 = sub i64 %i0, 1
  %tp = getelementptr inbounds [160 x i8], ptr %tmp, i64 0, i64 %i1
  store i8 %ch, ptr %tp
  store i64 %i1, ptr %idx
  store i128 %q, ptr %vv
  %more = icmp ne i128 %q, 0
  br i1 %more, label %loop, label %sign
sign:
  %pl = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 1
  %plus = load i1, ptr %pl
  %hasSign = or i1 %neg, %plus
  br i1 %hasSign, label %putsign, label %digitsOut
putsign:
  %si = load i64, ptr %idx
  %s1 = sub i64 %si, 1
  %sp = getelementptr inbounds [160 x i8], ptr %tmp, i64 0, i64 %s1
  %signch = select i1 %neg, i8 45, i8 43
  store i8 %signch, ptr %sp
  store i64 %s1, ptr %idx
  br label %digitsOut
digitsOut:
  %start = load i64, ptr %idx
  %len = sub i64 160, %start
  %startp = getelementptr inbounds [160 x i8], ptr %tmp, i64 0, i64 %start
  call void @rift_pad(ptr %b, ptr %startp, i64 %len, ptr %spec, i8 62)
  ret void
}

define internal void @rift_fmt_i(ptr %b, i128 %v, ptr %spec) {
entry:
  %neg = icmp slt i128 %v, 0
  %nv = sub i128 0, %v
  %mag = select i1 %neg, i128 %nv, i128 %v
  call void @rift_fmt_u(ptr %b, i128 %mag, i1 %neg, ptr %spec)
  ret void
}

define internal i64 @rift_utf8(i32 %cp, ptr %out) {
entry:
  %c1 = icmp ult i32 %cp, 128
  br i1 %c1, label %one, label %not1
one:
  %b0 = trunc i32 %cp to i8
  store i8 %b0, ptr %out
  ret i64 1
not1:
  %c2 = icmp ult i32 %cp, 2048
  br i1 %c2, label %two, label %not2
two:
  %h = lshr i32 %cp, 6
  %t0 = or i32 %h, 192
  %t0b = trunc i32 %t0 to i8
  store i8 %t0b, ptr %out
  %l = and i32 %cp, 63
  %t1 = or i32 %l, 128
  %t1b = trunc i32 %t1 to i8
  %p1 = getelementptr i8, ptr %out, i64 1
  store i8 %t1b, ptr %p1
  ret i64 2
not2:
  %c3 = icmp ult i32 %cp, 65536
  br i1 %c3, label %three, label %four
three:
  %a = lshr i32 %cp, 12
  %u0 = or i32 %a, 224
  %u0b = trunc i32 %u0 to i8
  store i8 %u0b, ptr %out
  %m = lshr i32 %cp, 6
  %m2 = and i32 %m, 63
  %u1 = or i32 %m2, 128
  %u1b = trunc i32 %u1 to i8
  %p3 = getelementptr i8, ptr %out, i64 1
  store i8 %u1b, ptr %p3
  %l3 = and i32 %cp, 63
  %u2 = or i32 %l3, 128
  %u2b = trunc i32 %u2 to i8
  %p4 = getelementptr i8, ptr %out, i64 2
  store i8 %u2b, ptr %p4
  ret i64 3
four:
  %f = lshr i32 %cp, 18
  %v0 = or i32 %f, 240
  %v0b = trunc i32 %v0 to i8
  store i8 %v0b, ptr %out
  %g = lshr i32 %cp, 12
  %g2 = and i32 %g, 63
  %v1 = or i32 %g2, 128
  %v1b = trunc i32 %v1 to i8
  %p5 = getelementptr i8, ptr %out, i64 1
  store i8 %v1b, ptr %p5
  %k = lshr i32 %cp, 6
  %k2 = and i32 %k, 63
  %v2 = or i32 %k2, 128
  %v2b = trunc i32 %v2 to i8
  %p6 = getelementptr i8, ptr %out, i64 2
  store i8 %v2b, ptr %p6
  %j = and i32 %cp, 63
  %v3 = or i32 %j, 128
  %v3b = trunc i32 %v3 to i8
  %p7 = getelementptr i8, ptr %out, i64 3
  store i8 %v3b, ptr %p7
  ret i64 4
}

define internal void @rift_fmt_char(ptr %b, i32 %cp, ptr %spec) {
entry:
  %rp = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 4
  %r = load i8, ptr %rp
  %numeric = icmp ne i8 %r, 0
  br i1 %numeric, label %num, label %chr
num:
  %wide = zext i32 %cp to i128
  call void @rift_fmt_u(ptr %b, i128 %wide, i1 false, ptr %spec)
  ret void
chr:
  %tmp = alloca [4 x i8]
  %n = call i64 @rift_utf8(i32 %cp, ptr %tmp)
  call void @rift_pad(ptr %b, ptr %tmp, i64 %n, ptr %spec, i8 60)
  ret void
}

; True when the text has none of '.', 'e' and 'n' (so "%g" output needs ".0" appended).
define internal i1 @rift_needs_point(ptr %s, i64 %n) {
entry:
  %i = alloca i64
  %found = alloca i1
  store i64 0, ptr %i
  store i1 false, ptr %found
  br label %cond
cond:
  %iv = load i64, ptr %i
  %more = icmp ult i64 %iv, %n
  br i1 %more, label %body, label %done
body:
  %p = getelementptr i8, ptr %s, i64 %iv
  %ch = load i8, ptr %p
  %isd = icmp eq i8 %ch, 46
  %ise = icmp eq i8 %ch, 101
  %isn = icmp eq i8 %ch, 110
  %a1 = or i1 %isd, %ise
  %any = or i1 %a1, %isn
  %f = load i1, ptr %found
  %nf = or i1 %f, %any
  store i1 %nf, ptr %found
  %next = add i64 %iv, 1
  store i64 %next, ptr %i
  br label %cond
done:
  %fv = load i1, ptr %found
  %r = xor i1 %fv, true
  ret i1 %r
}

; Floats: NaN, infinities, a fixed precision, or the shortest text that reads back exactly
; (with ".0" for whole numbers).
define internal void @rift_fmt_f(ptr %b, double %v, i1 %is32, ptr %spec) {
entry:
  %tmp = alloca [512 x i8]
  %n = alloca i32
  %pv = alloca i32
  %prec = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 3
  %precv = load i32, ptr %prec
  %plp = getelementptr inbounds %rift_spec, ptr %spec, i32 0, i32 1
  %plus = load i1, ptr %plp
  %isnan = fcmp uno double %v, %v
  br i1 %isnan, label %nanb, label %notnan
nanb:
  call void @rift_pad(ptr %b, ptr @rift_nan, i64 3, ptr %spec, i8 62)
  ret void
notnan:
  %abs = call double @llvm.fabs.f64(double %v)
  %isinf = fcmp oeq double %abs, 0x7FF0000000000000
  br i1 %isinf, label %infb, label %notinf
infb:
  %neg = fcmp olt double %v, 0.0
  %pinf = select i1 %plus, ptr @rift_pinf, ptr @rift_inf
  %pinfl = select i1 %plus, i64 4, i64 3
  %ip = select i1 %neg, ptr @rift_ninf, ptr %pinf
  %il = select i1 %neg, i64 4, i64 %pinfl
  call void @rift_pad(ptr %b, ptr %ip, i64 %il, ptr %spec, i8 62)
  ret void
notinf:
  %hasprec = icmp sge i32 %precv, 0
  br i1 %hasprec, label %precb, label %shortb
precb:
  %pfmt = select i1 %plus, ptr @rift_cfmt_plus_f, ptr @rift_cfmt_f
  %kp = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %tmp, i64 512, ptr %pfmt, i32 %precv, double %v)
  store i32 %kp, ptr %n
  br label %emit
shortb:
  ; A whole number below 1e16 prints all its digits ("50.0", not "5e+01").
  %fl = call double @llvm.floor.f64(double %v)
  %whole = fcmp oeq double %fl, %v
  %small = fcmp olt double %abs, 1.0e16
  %plain = and i1 %whole, %small
  %p0 = select i1 %plain, i32 17, i32 1
  store i32 %p0, ptr %pv
  br i1 %plain, label %found, label %search
search:
  %p = load i32, ptr %pv
  %atEnd = icmp sge i32 %p, 17
  br i1 %atEnd, label %found, label %try
try:
  %k = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %tmp, i64 512, ptr @rift_cfmt_g, i32 %p, double %v)
  %back = call double @strtod(ptr %tmp, ptr null)
  %backf = fptrunc double %back to float
  %vf = fptrunc double %v to float
  %eq32 = fcmp oeq float %backf, %vf
  %eq64 = fcmp oeq double %back, %v
  %same = select i1 %is32, i1 %eq32, i1 %eq64
  br i1 %same, label %found, label %next
next:
  %p1 = add i32 %p, 1
  store i32 %p1, ptr %pv
  br label %search
found:
  %pf = load i32, ptr %pv
  %gfmt = select i1 %plus, ptr @rift_cfmt_plus_g, ptr @rift_cfmt_g
  %k2 = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %tmp, i64 512, ptr %gfmt, i32 %pf, double %v)
  %k2w = sext i32 %k2 to i64
  %needs = call i1 @rift_needs_point(ptr %tmp, i64 %k2w)
  %room = icmp ult i64 %k2w, 510
  %append = and i1 %needs, %room
  br i1 %append, label %addpoint, label %keep
addpoint:
  %dotp = getelementptr i8, ptr %tmp, i64 %k2w
  store i8 46, ptr %dotp
  %k3 = add i64 %k2w, 1
  %zerop = getelementptr i8, ptr %tmp, i64 %k3
  store i8 48, ptr %zerop
  %k4 = add i64 %k2w, 2
  %k4w = trunc i64 %k4 to i32
  store i32 %k4w, ptr %n
  br label %emit
keep:
  store i32 %k2, ptr %n
  br label %emit
emit:
  %nn = load i32, ptr %n
  %nl = sext i32 %nn to i64
  call void @rift_pad(ptr %b, ptr %tmp, i64 %nl, ptr %spec, i8 62)
  ret void
}

define internal void @rift_flush(ptr %f, ptr %b, i1 %newline) {
entry:
  br i1 %newline, label %nl, label %write
nl:
  call void @rift_buf_put(ptr %b, ptr @rift_newline, i64 1)
  br label %write
write:
  %datap = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 0
  %lenp = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 1
  %capp = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 2
  %len = load i64, ptr %lenp
  %data = load ptr, ptr %datap
  %any = icmp ne i64 %len, 0
  br i1 %any, label %out, label %release
out:
  call i64 @fwrite(ptr %data, i64 1, i64 %len, ptr %f)
  br label %release
release:
  call void @free(ptr %data)
  store ptr null, ptr %datap
  store i64 0, ptr %lenp
  store i64 0, ptr %capp
  ret void
}

; The text and its newline under one lock of the stream, so lines of different threads don't mix.
define internal void @rift_write_str(ptr %f, ptr %s, i64 %n, i1 %newline) {
entry:
  call void @flockfile(ptr %f)
  call i64 @fwrite(ptr %s, i64 1, i64 %n, ptr %f)
  br i1 %newline, label %nl, label %done
nl:
  call i32 @fputc(i32 10, ptr %f)
  br label %done
done:
  call void @funlockfile(ptr %f)
  ret void
}

; Sleeps while the u32 at %p still holds %expected (FUTEX_WAIT_PRIVATE), and wakes up to %count
; sleepers (FUTEX_WAKE_PRIVATE). Spurious wake-ups are fine: callers check their condition again.
define internal void @rift_futex_wait(ptr %p, i32 %expected) {
entry:
  %r = call i64 (i64, ...) @syscall(i64 @@SYS_FUTEX@@, ptr %p, i32 128, i32 %expected, ptr null, ptr null, i32 0)
  ret void
}

; The same with a longest wait of %ns nanoseconds (a relative timeout).
define internal void @rift_futex_wait_for(ptr %p, i32 %expected, i64 %ns) {
entry:
  %ts = alloca { i64, i64 }, align 8
  %sec = udiv i64 %ns, 1000000000
  %rest = urem i64 %ns, 1000000000
  %sp = getelementptr inbounds { i64, i64 }, ptr %ts, i32 0, i32 0
  store i64 %sec, ptr %sp
  %np = getelementptr inbounds { i64, i64 }, ptr %ts, i32 0, i32 1
  store i64 %rest, ptr %np
  %r = call i64 (i64, ...) @syscall(i64 @@SYS_FUTEX@@, ptr %p, i32 128, i32 %expected, ptr %ts, ptr null, i32 0)
  ret void
}

define internal void @rift_futex_wake(ptr %p, i32 %count) {
entry:
  %r = call i64 (i64, ...) @syscall(i64 @@SYS_FUTEX@@, ptr %p, i32 129, i32 %count, ptr null, ptr null, i32 0)
  ret void
}

; Equal lengths first: different lengths never reach memcmp.
define internal i1 @rift_str_eq(ptr %a, i64 %an, ptr %b, i64 %bn) alwaysinline {
entry:
  %same = icmp eq i64 %an, %bn
  br i1 %same, label %bytes, label %no
bytes:
  %cmp = call i32 @memcmp(ptr %a, ptr %b, i64 %an)
  %zero = icmp eq i32 %cmp, 0
  ret i1 %zero
no:
  ret i1 false
}

; The stream a message goes to: stdout, or stderr when `err` is true.
define ptr @rift_stream(i1 %err) {
entry:
  %out = load ptr, ptr @stdout
  %er = load ptr, ptr @stderr
  %r = select i1 %err, ptr %er, ptr %out
  ret ptr %r
}

; Panics (spec 11): flush stdout, print `file:line:col: panic: message` on stderr, abort.
define internal void @rift_panic_buf(ptr %loc, ptr %msg) noreturn cold {
entry:
  %b = alloca %rift_buf
  store %rift_buf zeroinitializer, ptr %b
  %out = load ptr, ptr @stdout
  call i32 @fflush(ptr %out)
  %filep = getelementptr inbounds %rift_loc, ptr %loc, i32 0, i32 0
  %file = load ptr, ptr %filep
  %flen = call i64 @strlen(ptr %file)
  call void @rift_buf_put(ptr %b, ptr %file, i64 %flen)
  call void @rift_buf_put(ptr %b, ptr @rift_colon, i64 1)
  %linep = getelementptr inbounds %rift_loc, ptr %loc, i32 0, i32 1
  %line = load i32, ptr %linep
  %line128 = zext i32 %line to i128
  call void @rift_fmt_u(ptr %b, i128 %line128, i1 false, ptr @rift_plain_spec)
  call void @rift_buf_put(ptr %b, ptr @rift_colon, i64 1)
  %colp = getelementptr inbounds %rift_loc, ptr %loc, i32 0, i32 2
  %col = load i32, ptr %colp
  %col128 = zext i32 %col to i128
  call void @rift_fmt_u(ptr %b, i128 %col128, i1 false, ptr @rift_plain_spec)
  call void @rift_buf_put(ptr %b, ptr @rift_panic_prefix, i64 9)
  %mdatap = getelementptr inbounds %rift_buf, ptr %msg, i32 0, i32 0
  %mlenp = getelementptr inbounds %rift_buf, ptr %msg, i32 0, i32 1
  %mdata = load ptr, ptr %mdatap
  %mlen = load i64, ptr %mlenp
  %hasmsg = icmp ne i64 %mlen, 0
  %safe = select i1 %hasmsg, ptr %mdata, ptr @rift_colon
  %slen = select i1 %hasmsg, i64 %mlen, i64 0
  call void @rift_buf_put(ptr %b, ptr %safe, i64 %slen)
  call void @rift_buf_put(ptr %b, ptr @rift_newline, i64 1)
  %datap = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 0
  %lenp = getelementptr inbounds %rift_buf, ptr %b, i32 0, i32 1
  %data = load ptr, ptr %datap
  %blen = load i64, ptr %lenp
  ; std::log's hook (spec 31): log the panic and write and flush every queued record first, so
  ; the panic is the last line. Taken once, so a panic inside the hook doesn't run it again.
  %hook = atomicrmw xchg ptr @rift_panic_hook, ptr null seq_cst
  %has_hook = icmp ne ptr %hook, null
  br i1 %has_hook, label %run_hook, label %print
run_hook:
  call void %hook(ptr %safe, i64 %slen, ptr %loc)
  %out2 = load ptr, ptr @stdout
  call i32 @fflush(ptr %out2)
  br label %print
print:
  %err = load ptr, ptr @stderr
  call i64 @fwrite(ptr %data, i64 1, i64 %blen, ptr %err)
  call i32 @fflush(ptr %err)
  call void @abort()
  unreachable
}

@rift_panic_hook = internal global ptr null

; Sets the function a panic calls before printing its message: (message, length, location), see
; std::set_panic_hook.
define void @rift_set_panic_hook(ptr %hook) {
entry:
  store atomic ptr %hook, ptr @rift_panic_hook seq_cst, align 8
  ret void
}

define void @rift_panic_str(ptr %loc, ptr %s, i64 %n) noreturn cold {
entry:
  %b = alloca %rift_buf
  store %rift_buf zeroinitializer, ptr %b
  call void @rift_buf_put(ptr %b, ptr %s, i64 %n)
  call void @rift_panic_buf(ptr %loc, ptr %b)
  unreachable
}

; main's result must be a valid exit status (spec 04).
define i32 @rift_exit_status(i128 %v, ptr %loc) {
entry:
  %low = icmp slt i128 %v, 0
  %high = icmp sgt i128 %v, 255
  %bad = or i1 %low, %high
  br i1 %bad, label %badb, label %good
badb:
  %b = alloca %rift_buf
  store %rift_buf zeroinitializer, ptr %b
  call void @rift_buf_put(ptr %b, ptr @rift_main_returned, i64 14)
  call void @rift_fmt_i(ptr %b, i128 %v, ptr @rift_plain_spec)
  call void @rift_buf_put(ptr %b, ptr @rift_not_valid, i64 __NOT_VALID_LEN__)
  call void @rift_panic_buf(ptr %loc, ptr %b)
  unreachable
good:
  %r = trunc i128 %v to i32
  ret i32 %r
}

; The program's arguments (spec 04): raw for std::env::raw_args, and as std::str for main(args).
@rift_argc = internal global i32 0
@rift_argv = internal global ptr null

define i32 @rift_arg_count() {
entry:
  %c = load i32, ptr @rift_argc
  ret i32 %c
}

define ptr @rift_arg_values() {
entry:
  %v = load ptr, ptr @rift_argv
  ret ptr %v
}

; Whether the n bytes at s are well-formed UTF-8.
define internal i1 @rift_utf8_valid(ptr %s, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %advance ]
  %more = icmp ult i64 %i, %n
  br i1 %more, label %lead, label %yes
lead:
  %bp = getelementptr i8, ptr %s, i64 %i
  %b = load i8, ptr %bp
  %ascii = icmp ult i8 %b, 128
  br i1 %ascii, label %one, label %multi
one:
  br label %advance
multi:
  %m2 = and i8 %b, 224
  %is2 = icmp eq i8 %m2, 192
  %ge2 = icmp uge i8 %b, 194
  %two = and i1 %is2, %ge2
  %m3 = and i8 %b, 240
  %three = icmp eq i8 %m3, 224
  %m4 = and i8 %b, 248
  %is4 = icmp eq i8 %m4, 240
  %le4 = icmp ule i8 %b, 244
  %four = and i1 %is4, %le4
  %l3 = select i1 %three, i64 3, i64 4
  %l2 = select i1 %two, i64 2, i64 %l3
  %known = or i1 %two, %three
  %known2 = or i1 %known, %four
  br i1 %known2, label %fits, label %no
fits:
  %end = add i64 %i, %l2
  %inside = icmp ule i64 %end, %n
  br i1 %inside, label %cont, label %no
cont:
  %j = phi i64 [ 1, %fits ], [ %j1, %contnext ]
  %done = icmp eq i64 %j, %l2
  br i1 %done, label %multidone, label %contbyte
contbyte:
  %k = add i64 %i, %j
  %cp = getelementptr i8, ptr %s, i64 %k
  %c = load i8, ptr %cp
  %cm = and i8 %c, 192
  %isc = icmp eq i8 %cm, 128
  br i1 %isc, label %contnext, label %no
contnext:
  %j1 = add i64 %j, 1
  br label %cont
multidone:
  br label %advance
advance:
  %step = phi i64 [ 1, %one ], [ %l2, %multidone ]
  %next = add i64 %i, %step
  br label %loop
yes:
  ret i1 true
no:
  ret i1 false
}

; Called by the entry point: keeps argc/argv and returns argv as an array of std::str.
define ptr @rift_main_args(i32 %argc, ptr %argv) {
entry:
  store i32 %argc, ptr @rift_argc
  store ptr %argv, ptr @rift_argv
  %n = sext i32 %argc to i64
  %bytes = mul i64 %n, 16
  %size = add i64 %bytes, 16
  %arr = call ptr @malloc(i64 %size)
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %store ]
  %more = icmp slt i64 %i, %n
  br i1 %more, label %body, label %done
body:
  %pp = getelementptr ptr, ptr %argv, i64 %i
  %p = load ptr, ptr %pp
  %len = call i64 @strlen(ptr %p)
  %ok = call i1 @rift_utf8_valid(ptr %p, i64 %len)
  br i1 %ok, label %store, label %bad
store:
  %slot = getelementptr { ptr, i64 }, ptr %arr, i64 %i
  %f0 = getelementptr { ptr, i64 }, ptr %slot, i32 0, i32 0
  store ptr %p, ptr %f0
  %f1 = getelementptr { ptr, i64 }, ptr %slot, i32 0, i32 1
  store i64 %len, ptr %f1
  %next = add i64 %i, 1
  br label %loop
bad:
  %err = load ptr, ptr @stderr
  %w = call i64 @fwrite(ptr @rift_bad_arg, i64 1, i64 __BAD_ARG_LEN__, ptr %err)
  call void @exit(i32 1)
  unreachable
done:
  ret ptr %arr
}
)IR";

} // namespace

std::string llvm_runtime_ir() {
    const std::string bad_arg = "error: a command-line argument is not valid UTF-8 (read it with std::env::raw_args())\n";
    std::string out = kTypesAndDeclarations;
    out += bytes_global("rift_oom", "rift: out of memory\n");
    out += cstring_global("rift_true", "true");
    out += cstring_global("rift_false", "false");
    out += cstring_global("rift_nan", "NaN");
    out += cstring_global("rift_inf", "inf");
    out += cstring_global("rift_ninf", "-inf");
    out += cstring_global("rift_pinf", "+inf");
    out += cstring_global("rift_cfmt_g", "%.*g");
    out += cstring_global("rift_cfmt_plus_g", "%+.*g");
    out += cstring_global("rift_cfmt_f", "%.*f");
    out += cstring_global("rift_cfmt_plus_f", "%+.*f");
    out += bytes_global("rift_newline", "\n");
    out += bytes_global("rift_colon", ":");
    out += bytes_global("rift_panic_prefix", ": panic: ");
    out += bytes_global("rift_main_returned", "main returned ");
    out += bytes_global("rift_not_valid", ", which is not a valid exit status (0 to 255)");
    std::string body = kBuffers;
    // The length of the exit-status suffix, written in the IR below.
    out += bytes_global("rift_bad_arg", bad_arg);
    std::string placeholder = "__NOT_VALID_LEN__";
    std::string len = std::to_string(std::string(", which is not a valid exit status (0 to 255)").size());
    for (std::size_t at = body.find(placeholder); at != std::string::npos; at = body.find(placeholder)) {
        body.replace(at, placeholder.size(), len);
    }
    for (std::size_t at = body.find("__BAD_ARG_LEN__"); at != std::string::npos; at = body.find("__BAD_ARG_LEN__")) {
        body.replace(at, 15, std::to_string(bad_arg.size()));
    }
    return out + body;
}

} // namespace rift
