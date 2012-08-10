CXX = clang++
CFLAGS = -fno-rtti -std=c++0x -stdlib=libc++ -Wall -Werror

LLVM_SRC_PATH = ../clang/llvm
LLVM_BUILD_PATH = ../clang/build

LLVM_BIN_PATH = $(LLVM_BUILD_PATH)/Debug+Asserts/bin
#LLVM_LIBS=asmparser core mc support
LLVM_LIBS=all
LLVM_CONFIG_COMMAND = $(LLVM_BIN_PATH)/llvm-config --cxxflags --ldflags \
                                        --libs $(LLVM_LIBS)
CLANG_BUILD_FLAGS = -I$(LLVM_SRC_PATH)/tools/clang/include \
                                      -I$(LLVM_BUILD_PATH)/tools/clang/include

CLANGLIBS = \
	-lclangTooling -lclangFrontend -lclangDriver \
	-lclangSerialization -lclangParse -lclangSema \
	-lclangAnalysis -lclangEdit -lclangAST \
	-lclangLex -lclangBasic -lclangRewrite

all: fix-unused-args

fix-unused-args: fix-unused-args.cpp
	$(CXX) fix-unused-args.cpp $(CFLAGS) -o fix-unused-args \
	$(CLANG_BUILD_FLAGS) $(CLANGLIBS) `$(LLVM_CONFIG_COMMAND)`

clean:
	rm -rf *.o *.ll fix-unused-args

