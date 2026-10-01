/**
 * @file msvc_avx2_reduction_repro.cpp
 * @brief The "lambda sum" mystery of the arrangement round (19.09.2026), traced: an MSVC code-generation fault.
 *
 * Not part of any target. Build and run it by hand (x64 Native Tools prompt):
 * @code
 *   cl /nologo /std:c++20 /O2 /arch:AVX2 msvc_avx2_reduction_repro.cpp && msvc_avx2_reduction_repro
 * @endcode
 *
 * **What happened.** The arrangement round wrote Form.cpp's fitTemplate with a lambda that sums the eight slot
 * lengths, called in the conditions of two nested loops that grow the slots 16 bars at a time. In the Release
 * build every track longer than 256 bars came out at 256: no slot ever grew. Rewritten with a running total it
 * worked, and the cause was left open (docs/rounds/2026-09.md, round "polish").
 *
 * **What it is not.** Undefined behaviour was the first suspect. The code reads eight ints through a pointer it
 * was handed, writes one of them through the same pointer, and indexes the templates' tables inside their bounds;
 * nothing is uninitialised, nothing dangles, no two pointers of different types alias. The same source is correct
 * at /Od, at /O2 without /arch:AVX2 (SSE2 and /arch:AVX alike), and at /O2 /arch:AVX2 with /Ob0 (no inlining);
 * the reconstruction of the whole fitTemplate was correct at /O1 /arch:AVX2 as well, this function alone is not.
 *
 * **What it is.** At /O2 /arch:AVX2 (and /arch:AVX512) MSVC 19.51.36257 (Visual Studio 18 2026) inlines the lambda
 * and vectorises its sum as one ymm load and a horizontal reduction:
 * @code
 *   vmovdqu ymm0, [bars]      ; the eight slots
 *   vphaddd ymm1, ymm0, ymm0
 *   vphaddd ymm2, ymm1, ymm1  ; ymm2: the *reduced* vector (each 128-bit half holds its half's sum four times)
 *   vextracti128 ... vpaddd ... vmovd eax  ; total()
 * @endcode
 * For the second call (the inner loop's condition) the compiler reuses the load it thinks it still has, but the
 * register it reuses is ymm2 -- the reduced vector, not the loaded one -- and reduces it again:
 * @code
 *   vphaddd ymm0, ymm2, ymm2  ; a third horizontal add
 *   vphaddd ymm2, ymm0, ymm0  ; a fourth
 * @endcode
 * The inner condition therefore reads four times the true sum (4 x 256 = 1024), is never below the target, and
 * the growth loop leaves after its first test. Where a store happened the compiler does reload ymm2 from memory,
 * which is why the fault needs the shape "reduction, then the same reduction again with no store between".
 *
 * **How to find it again.** A correct 8 x int32 reduction chains exactly two horizontal adds. Scanning the /FAs
 * listings of the whole core, phos_render and the self test for a chain of three or more (a script of the
 * polish round, work\phosphene-work\scratch\polish\repro\scan_hadd.py) finds this function and nothing else in the
 * project today: the code as it stands does not contain the pattern.
 *
 * **What to do.** Nothing in the code: the running total the arrangement round wrote is correct and is what
 * Form.cpp keeps. testArrangement (a) builds every length 128..320 of every style and would fail at once if a
 * form came out at the wrong length again. The repro below is small enough to report to Microsoft
 * (Developer Community): expected output "fit: 272 (expected 272)"; MSVC 19.51 at /O2 /arch:AVX2 prints 4096 --
 * the return statement's sum is reduced four more times as well (16 x 256). A smaller loop of the same shape
 * without the table-driven slot does not trip the vectoriser at /O2 /arch:AVX2 (it computed 20 of 20), so the
 * fault depends on the loop nest the optimiser sees, not only on the lambda.
 */
#include <cstdio>

/**
 * @brief The fitTemplate shape of the arrangement round (Full-On's template, target 272 bars): the lambda sum in
 *        both conditions, with a table-driven slot to grow. Expected 272.
 */
__declspec(noinline) int fit(int target)
{
    static const int kBars[8] = { 32, 32, 16, 32, 32, 32, 48, 32 };
    static const int kGrow[8] = { 6, 3, 4, 1, 3, 4, 1, -1 };
    static const int kCap[8] = { 64, 48, 48, 48, 64, 64, 64, 0 };
    int bars[8];
    for (int k = 0; k < 8; ++k) bars[k] = kBars[k];
    auto total = [&]() { int s = 0; for (int k = 0; k < 8; ++k) s += bars[k]; return s; };
    for (bool moved = true; total() < target && moved;) {
        moved = false;
        for (int g = 0; g < 8 && kGrow[g] >= 0 && total() < target; ++g)
            if (bars[kGrow[g]] + 16 <= kCap[g]) { bars[kGrow[g]] += 16; moved = true; }
    }
    return total();
}

int main()
{
    const int f = fit(272);
    std::printf("fit: %d (expected 272)\n", f);
    return f == 272 ? 0 : 1;
}
