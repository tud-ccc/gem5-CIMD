// Coherence test for CIM RowOps with caches: operands are written by the CPU
// (dirty in the caches), a RowOp computes in DRAM, and the result is read back
// through the caches. A second round changes the inputs to catch stale lines.
// Run with a cached config, e.g.:
//   gem5.opt configs/cim/cim_cpu.py --cmd "cim_test_cache_coherence 100000"
// usage: cim_test_cache_coherence [n_elems]
#include "cim_core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace pim_core;

int main( int argc, char** argv )
{
  const size_t n = argc > 1 ? std::strtoul( argv[1], nullptr, 10 ) : 64;
  const size_t bits = 32;
  auto* a = static_cast<int32_t*>( cim_malloc( n * sizeof( int32_t ), 0 ) );
  auto* b = static_cast<int32_t*>( cim_malloc( n * sizeof( int32_t ), 0 ) );
  auto* m = static_cast<int32_t*>( cim_malloc( n * sizeof( int32_t ), 0 ) );
  auto* d = static_cast<int32_t*>( cim_malloc( n * sizeof( int32_t ), 0 ) );
  size_t wrong = 0;
  for ( int round = 0; round < 2; ++round )
  {
    for ( size_t i = 0; i < n; ++i )
    {
      a[i] = static_cast<int32_t>( i * 3 + round * 1000 );
      b[i] = static_cast<int32_t>( i + 7 * round );
      m[i] = ( i + round ) % 2 ? -1 : 0;
      d[i] = -12345; // stale value that must not survive
    }
    rowadd( d, a, b, n, bits );
    for ( size_t i = 0; i < n; ++i )
      wrong += d[i] != a[i] + b[i];
    rowif_else( d, a, b, m, n, bits );
    for ( size_t i = 0; i < n; ++i )
      wrong += d[i] != ( m[i] ? a[i] : b[i] );
    rowadd( d, d, b, n, bits ); // in place: overlapping operands
    for ( size_t i = 0; i < n; ++i )
      wrong += d[i] != ( m[i] ? a[i] : b[i] ) + b[i];
  }
  std::printf( "cim_test_cache_coherence n=%zu: %zu wrong\n", n, wrong );
  return wrong != 0;
}
