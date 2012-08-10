cd clang
mkdir build
cd build
../llvm/configure
CXXFLAGS="-std=c++0x -stdlib=libc++" make -j 7
