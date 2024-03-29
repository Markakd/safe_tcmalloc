# source commit by zhenpeng: ff8cf4aa6d70cb8cacdb69fedb2fb501ea54c4a0
rm -rf static
mkdir static

cp ../abseil-cpp/absl/strings/libabsl_strings_internal.a static
cd static
ar x libabsl_strings_internal.a
mv escaping.cc.o ee.cc.o
rm libabsl_strings_internal.a
cd ..
cp CMakeFiles/tcmalloc.dir/tcmalloc.cc.o CMakeFiles/tcmalloc.dir/allocation_sample.cc.o CMakeFiles/tcmalloc.dir/cpu_cache.cc.o CMakeFiles/tcmalloc.dir/global_stats.cc.o CMakeFiles/tcmalloc.dir/sampled_allocation.cc.o CMakeFiles/tcmalloc.dir/lifetime_based_allocator.cc.o CMakeFiles/tcmalloc.dir/experimental_cfl_aware_size_class.cc.o ../abseil-cpp/absl/debugging/libabsl_leak_check.a ../abseil-cpp/absl/status/libabsl_statusor.a internal/libtcmalloc_memory_stats.a internal/libtcmalloc_residency.a libtcmalloc_common.a  ../abseil-cpp/absl/log/libabsl_log_internal_format.a ../abseil-cpp/absl/log/libabsl_log_internal_globals.a libtcmalloc_experiment.a  ../abseil-cpp/absl/status/libabsl_status.a ../abseil-cpp/absl/strings/libabsl_cord.a ../abseil-cpp/absl/strings/libabsl_cordz_info.a ../abseil-cpp/absl/strings/libabsl_cord_internal.a ../abseil-cpp/absl/strings/libabsl_cordz_functions.a ../abseil-cpp/absl/profiling/libabsl_exponential_biased.a ../abseil-cpp/absl/strings/libabsl_cordz_handle.a ../abseil-cpp/absl/base/libabsl_strerror.a ../abseil-cpp/absl/synchronization/libabsl_synchronization.a ../abseil-cpp/absl/debugging/libabsl_symbolize.a ../abseil-cpp/absl/debugging/libabsl_demangle_internal.a ../abseil-cpp/absl/synchronization/libabsl_graphcycles_internal.a ../abseil-cpp/absl/hash/libabsl_hash.a ../abseil-cpp/absl/types/libabsl_bad_variant_access.a ../abseil-cpp/absl/hash/libabsl_city.a ../abseil-cpp/absl/hash/libabsl_low_level_hash.a internal/libtcmalloc_numa.a internal/libtcmalloc_environment.a internal/libtcmalloc_cache_topology.a internal/libtcmalloc_percpu.a internal/libtcmalloc_util.a internal/libtcmalloc_mincore.a internal/libtcmalloc_logging.a libtcmalloc_malloc_extension.a ../abseil-cpp/absl/types/libabsl_bad_optional_access.a ../abseil-cpp/absl/base/libabsl_malloc_internal.a ../abseil-cpp/absl/debugging/libabsl_stacktrace.a ../abseil-cpp/absl/debugging/libabsl_debugging_internal.a ../abseil-cpp/absl/strings/libabsl_str_format_internal.a ../abseil-cpp/absl/time/libabsl_time.a ../abseil-cpp/absl/strings/libabsl_strings.a  ../abseil-cpp/absl/base/libabsl_base.a ../abseil-cpp/absl/base/libabsl_spinlock_wait.a  ../abseil-cpp/absl/base/libabsl_throw_delegate.a ../abseil-cpp/absl/base/libabsl_raw_logging_internal.a ../abseil-cpp/absl/base/libabsl_log_severity.a ../abseil-cpp/absl/numeric/libabsl_int128.a ../abseil-cpp/absl/time/libabsl_civil_time.a ../abseil-cpp/absl/time/libabsl_time_zone.a static

cp ../abseil-cpp/absl/strings/CMakeFiles/absl_strings_internal.dir/internal/escaping.cc.o static/ee.cc.o
# cp ../abseil-cpp/absl/strings/CMakeFiles/absl_strings.dir/escaping.cc.o static
cd static
for i in `ls *.a`; do ar x $i;done
ar rcs libtcmalloc.a *.o

cd ..

gcc gprof.c -c
cp gprof.o ./static
