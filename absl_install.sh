set -eux
# we tend to get absl source codes in the same dir level with tcmalloc/
cd ..
# pull the codes
git clone https://github.com/abseil/abseil-cpp.git
# set up the version
cd abseil-cpp
git checkout 5937b7f9d123e6b01064fdb488d6c96d28d99a75 # we checkout to the version of Sept. 19th 2022, absl

# build absl to .so to /usr/local/lib
cmake . -DBUILD_SHARED_LIBS=ON -DCMAKE_CXX_STANDARD=17 -DCMAKE_MODULE_LINKER_FLAGS="-Wl,--no-undefined" && make -j`nproc` && sudo make install
