#pragma once
// The raw SIMD intrinsics (SIMD, "Raw intrinsics"): `std::simd::x86::mm256_shuffle_epi8(a, b)`,
// `std::simd::arm::vqtbl1q_u8(t, i)`. Each one is an LLVM intrinsic under the vendor's name
// without the leading underscores; it takes and returns Rift vector types.

#include <string_view>
#include <vector>

namespace rift::simd {

// The shape of a parameter or result.
struct Shape {
    enum class Kind {
        int_vector,     // an integer vector of `bits` bits, any lane type (`__m256i`)
        float_vector,   // a vector of `lanes` floats of `lane_bits` (`__m256` is 8 × 32)
        scalar_int,     // an integer of `lane_bits`
        scalar_float,   // a float of `lane_bits`
        same_as_first,  // the result: the type of the first argument
    };
    Kind kind      = Kind::int_vector;
    int  bits      = 0;   // int_vector: the vector's width
    int  lanes     = 0;   // float_vector
    int  lane_bits = 0;   // float_vector, scalars; int_vector results: the lane width
    bool is_signed = false;   // scalar_int and int_vector results
};

// One intrinsic: its arch, the CPU feature it needs, the LLVM intrinsic and its LLVM types.
struct Intrinsic {
    std::string_view   arch;        // "x86" or "arm"
    std::string_view   name;        // vendor's name without underscores
    std::string_view   feature;     // std::build::cpu_feature it needs
    std::string_view   llvm_name;   // the LLVM intrinsic
    std::vector<Shape> params;      // the Rift arguments
    Shape              result;
    // LLVM's types of the arguments and result, as (lanes, lane bits, float): lanes 0 = scalar.
    struct LlvmType {
        int  lanes;
        int  lane_bits;
        bool is_float;
    };
    std::vector<LlvmType> llvm_params;
    LlvmType              llvm_result;
    bool                  truncate_result = false;   // the LLVM result is wider than the Rift one
};

inline Shape iv(int bits) { return Shape{Shape::Kind::int_vector, bits, 0, 0, false}; }
inline Shape iv_result(int bits, int lane_bits, bool is_signed) { return Shape{Shape::Kind::int_vector, bits, 0, lane_bits, is_signed}; }
inline Shape fv(int lanes, int lane_bits) { return Shape{Shape::Kind::float_vector, 0, lanes, lane_bits, false}; }
inline Shape si(int bits, bool is_signed = false) { return Shape{Shape::Kind::scalar_int, 0, 0, bits, is_signed}; }
inline Shape sf(int bits) { return Shape{Shape::Kind::scalar_float, 0, 0, bits, false}; }
inline Shape first() { return Shape{Shape::Kind::same_as_first, 0, 0, 0, false}; }

inline const std::vector<Intrinsic>& intrinsics() {
    using T = Intrinsic::LlvmType;
    static const std::vector<Intrinsic> table = {
        // --- x86: SSE to AVX-512 -----------------------------------------------------------
        {"x86", "mm_shuffle_epi8", "ssse3", "llvm.x86.ssse3.pshuf.b.128", {iv(128), iv(128)}, first(), {T{16, 8, false}, T{16, 8, false}}, T{16, 8, false}},
        {"x86", "mm256_shuffle_epi8", "avx2", "llvm.x86.avx2.pshuf.b", {iv(256), iv(256)}, first(), {T{32, 8, false}, T{32, 8, false}}, T{32, 8, false}},
        {"x86", "mm_movemask_epi8", "sse2", "llvm.x86.sse2.pmovmskb.128", {iv(128)}, si(32, true), {T{16, 8, false}}, T{0, 32, false}},
        {"x86", "mm256_movemask_epi8", "avx2", "llvm.x86.avx2.pmovmskb", {iv(256)}, si(32, true), {T{32, 8, false}}, T{0, 32, false}},
        {"x86", "mm_movemask_ps", "sse", "llvm.x86.sse.movmsk.ps", {fv(4, 32)}, si(32, true), {T{4, 32, true}}, T{0, 32, false}},
        {"x86", "mm256_movemask_ps", "avx", "llvm.x86.avx.movmsk.ps.256", {fv(8, 32)}, si(32, true), {T{8, 32, true}}, T{0, 32, false}},
        {"x86", "mm256_permutevar8x32_epi32", "avx2", "llvm.x86.avx2.permd", {iv(256), iv(256)}, first(), {T{8, 32, false}, T{8, 32, false}}, T{8, 32, false}},
        {"x86", "mm256_permutevar8x32_ps", "avx2", "llvm.x86.avx2.permps", {fv(8, 32), iv(256)}, fv(8, 32), {T{8, 32, true}, T{8, 32, false}}, T{8, 32, true}},
        {"x86", "mm256_madd_epi16", "avx2", "llvm.x86.avx2.pmadd.wd", {iv(256), iv(256)}, iv_result(256, 32, true), {T{16, 16, false}, T{16, 16, false}}, T{8, 32, false}},
        {"x86", "mm256_maddubs_epi16", "avx2", "llvm.x86.avx2.pmadd.ub.sw", {iv(256), iv(256)}, iv_result(256, 16, true), {T{32, 8, false}, T{32, 8, false}}, T{16, 16, false}},
        {"x86", "mm_sad_epu8", "sse2", "llvm.x86.sse2.psad.bw", {iv(128), iv(128)}, iv_result(128, 64, false), {T{16, 8, false}, T{16, 8, false}}, T{2, 64, false}},
        {"x86", "mm256_sad_epu8", "avx2", "llvm.x86.avx2.psad.bw", {iv(256), iv(256)}, iv_result(256, 64, false), {T{32, 8, false}, T{32, 8, false}}, T{4, 64, false}},
        {"x86", "mm_mulhrs_epi16", "ssse3", "llvm.x86.ssse3.pmul.hr.sw.128", {iv(128), iv(128)}, first(), {T{8, 16, false}, T{8, 16, false}}, T{8, 16, false}},
        {"x86", "mm256_mulhrs_epi16", "avx2", "llvm.x86.avx2.pmul.hr.sw", {iv(256), iv(256)}, first(), {T{16, 16, false}, T{16, 16, false}}, T{16, 16, false}},
        {"x86", "mm_packs_epi32", "sse2", "llvm.x86.sse2.packssdw.128", {iv(128), iv(128)}, iv_result(128, 16, true), {T{4, 32, false}, T{4, 32, false}}, T{8, 16, false}},
        {"x86", "mm_packus_epi16", "sse2", "llvm.x86.sse2.packuswb.128", {iv(128), iv(128)}, iv_result(128, 8, false), {T{8, 16, false}, T{8, 16, false}}, T{16, 8, false}},
        {"x86", "mm256_packs_epi32", "avx2", "llvm.x86.avx2.packssdw", {iv(256), iv(256)}, iv_result(256, 16, true), {T{8, 32, false}, T{8, 32, false}}, T{16, 16, false}},
        {"x86", "mm256_packus_epi16", "avx2", "llvm.x86.avx2.packuswb", {iv(256), iv(256)}, iv_result(256, 8, false), {T{16, 16, false}, T{16, 16, false}}, T{32, 8, false}},
        {"x86", "mm_minpos_epu16", "sse4_1", "llvm.x86.sse41.phminposuw", {iv(128)}, first(), {T{8, 16, false}}, T{8, 16, false}},
        {"x86", "mm_hadd_ps", "sse3", "llvm.x86.sse3.hadd.ps", {fv(4, 32), fv(4, 32)}, fv(4, 32), {T{4, 32, true}, T{4, 32, true}}, T{4, 32, true}},
        {"x86", "mm256_hadd_ps", "avx", "llvm.x86.avx.hadd.ps.256", {fv(8, 32), fv(8, 32)}, fv(8, 32), {T{8, 32, true}, T{8, 32, true}}, T{8, 32, true}},
        {"x86", "mm_rsqrt_ps", "sse", "llvm.x86.sse.rsqrt.ps", {fv(4, 32)}, fv(4, 32), {T{4, 32, true}}, T{4, 32, true}},
        {"x86", "mm256_rsqrt_ps", "avx", "llvm.x86.avx.rsqrt.ps.256", {fv(8, 32)}, fv(8, 32), {T{8, 32, true}}, T{8, 32, true}},
        {"x86", "mm_rcp_ps", "sse", "llvm.x86.sse.rcp.ps", {fv(4, 32)}, fv(4, 32), {T{4, 32, true}}, T{4, 32, true}},
        {"x86", "mm256_rcp_ps", "avx", "llvm.x86.avx.rcp.ps.256", {fv(8, 32)}, fv(8, 32), {T{8, 32, true}}, T{8, 32, true}},
        {"x86", "mm_crc32_u8", "sse4_2", "llvm.x86.sse42.crc32.32.8", {si(32), si(8)}, si(32), {T{0, 32, false}, T{0, 8, false}}, T{0, 32, false}},
        {"x86", "mm_crc32_u32", "sse4_2", "llvm.x86.sse42.crc32.32.32", {si(32), si(32)}, si(32), {T{0, 32, false}, T{0, 32, false}}, T{0, 32, false}},
        {"x86", "mm_crc32_u64", "sse4_2", "llvm.x86.sse42.crc32.64.64", {si(64), si(64)}, si(64), {T{0, 64, false}, T{0, 64, false}}, T{0, 64, false}},
        {"x86", "mm512_sad_epu8", "avx512bw", "llvm.x86.avx512.psad.bw.512", {iv(512), iv(512)}, iv_result(512, 64, false), {T{64, 8, false}, T{64, 8, false}}, T{8, 64, false}},
        // --- ARM: NEON and SVE --------------------------------------------------------------
        {"arm", "vqtbl1q_u8", "neon", "llvm.aarch64.neon.tbl1.v16i8", {iv(128), iv(128)}, first(), {T{16, 8, false}, T{16, 8, false}}, T{16, 8, false}},
        {"arm", "vaddvq_u8", "neon", "llvm.aarch64.neon.uaddv.i32.v16i8", {iv(128)}, si(8), {T{16, 8, false}}, T{0, 32, false}, true},
        {"arm", "vaddvq_f32", "neon", "llvm.aarch64.neon.faddv.f32.v4f32", {fv(4, 32)}, sf(32), {T{4, 32, true}}, T{0, 32, true}},
        {"arm", "vmaxvq_f32", "neon", "llvm.aarch64.neon.fmaxv.f32.v4f32", {fv(4, 32)}, sf(32), {T{4, 32, true}}, T{0, 32, true}},
        {"arm", "vminvq_u8", "neon", "llvm.aarch64.neon.uminv.i32.v16i8", {iv(128)}, si(8), {T{16, 8, false}}, T{0, 32, false}, true},
        {"arm", "vrecpeq_f32", "neon", "llvm.aarch64.neon.frecpe.v4f32", {fv(4, 32)}, fv(4, 32), {T{4, 32, true}}, T{4, 32, true}},
        {"arm", "vrsqrteq_f32", "neon", "llvm.aarch64.neon.frsqrte.v4f32", {fv(4, 32)}, fv(4, 32), {T{4, 32, true}}, T{4, 32, true}},
        {"arm", "vpaddq_f32", "neon", "llvm.aarch64.neon.faddp.v4f32", {fv(4, 32), fv(4, 32)}, fv(4, 32), {T{4, 32, true}, T{4, 32, true}}, T{4, 32, true}},
        {"arm", "vcntq_u8", "neon", "llvm.ctpop.v16i8", {iv(128)}, first(), {T{16, 8, false}}, T{16, 8, false}},
    };
    return table;
}

inline const Intrinsic* find_intrinsic(std::string_view arch, std::string_view name) {
    for (const Intrinsic& i : intrinsics()) {
        if (i.arch == arch && i.name == name) return &i;
    }
    return nullptr;
}

} // namespace rift::simd
