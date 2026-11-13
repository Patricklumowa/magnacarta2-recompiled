// The Windows C runtime lacks C23 roundevenf. ReXGlue's vector rounding uses
// this symbol; SSE4.1 preserves ties-to-even independently of the host mode.
#include <smmintrin.h>

extern "C" __attribute__((target("sse4.1"))) float roundevenf(float value) {
  return _mm_cvtss_f32(_mm_round_ss(_mm_setzero_ps(), _mm_set_ss(value),
                                  _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
}
