TCPATH="`pwd`/.."
echo $TCPATH
clang++ test.cc -std=c++17  \
-I$TCPATH \
-o test.out -ltcmalloc_tcmalloc
