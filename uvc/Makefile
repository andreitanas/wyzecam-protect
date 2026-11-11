# make            host build (for tests on the Mac/Linux box)
# make camera     static mipsel binary for the Wyze Cam v2 (Ingenic T20)
# make test       host tests

.DEFAULT_GOAL := all

MBEDTLS := third_party/mbedtls
CJSON := third_party/cjson
NCNN := third_party/ncnn
STB := third_party/stb
VOAAC := third_party/vo-aacenc
OPUS := third_party/opus
# libopus's own source lists (make syntax); fixed-point build, no float API
# (on the T20 the float build is a quarter slower, and -O3 an eighth faster than -O2)
-include $(OPUS)/celt_sources.mk $(OPUS)/silk_sources.mk $(OPUS)/opus_sources.mk
OPUS_SRC := $(addprefix $(OPUS)/,$(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FIXED) $(OPUS_SOURCES))
OPUS_CFLAGS := -std=gnu11 -O3 -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DHAVE_LRINTF \
	-DVAR_ARRAYS -DHAVE_STDINT_H -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/silk \
	-I$(OPUS)/silk/fixed -I$(OPUS)/src -ffunction-sections -fdata-sections -w

SRC := $(wildcard src/*.c) $(wildcard src/*.cpp)
DEP_SRC := $(wildcard $(MBEDTLS)/library/*.c) $(CJSON)/cJSON.c \
	$(VOAAC)/common/cmnMemory.c $(wildcard $(VOAAC)/aacenc/basic_op/*.c) \
	$(wildcard $(VOAAC)/aacenc/src/*.c) $(OPUS_SRC)
CFLAGS_COMMON := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Os \
	-Isrc -I$(MBEDTLS)/include -I$(CJSON) -I$(STB) -I$(VOAAC)/common/include -I$(OPUS)/include \
	-ffunction-sections -fdata-sections -MMD -MP
DEP_CFLAGS := -std=gnu11 -Os -I$(MBEDTLS)/include -I$(MBEDTLS)/library -I$(CJSON) \
	-I$(VOAAC)/common/include -I$(VOAAC)/aacenc/inc -I$(VOAAC)/aacenc/basic_op \
	-ffunction-sections -fdata-sections -w

# libopus objects take its flags instead of DEP_CFLAGS
build/host/$(OPUS)/%.o build/camera/$(OPUS)/%.o: DEP_CFLAGS := $(OPUS_CFLAGS)

HOST_CC ?= cc
HOST_CXX ?= c++
CAMERA_CC ?= zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2
CAMERA_CXX ?= zig c++ -target mipsel-linux-musleabihf -mcpu=mips32r2

# ncnn (object detector inference), built with CMake per target.
NCNN_OPTS := -DCMAKE_BUILD_TYPE=Release -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF -DNCNN_THREADS=OFF \
	-DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF \
	-DNCNN_BUILD_TESTS=OFF -DNCNN_SHARED_LIB=OFF -DNCNN_SIMPLEOCV=OFF -DNCNN_SIMPLESTL=OFF \
	-DNCNN_MSA=OFF -DNCNN_MMI=OFF -DNCNN_DISABLE_EXCEPTION=ON -DNCNN_DISABLE_RTTI=ON
NCNN_HOST := build/ncnn-host/src/libncnn.a
NCNN_CAMERA := build/ncnn-camera/src/libncnn.a
CXXFLAGS_COMMON := -std=c++11 -Wall -Wextra -Os -fno-exceptions -fno-rtti -Isrc \
	-I$(NCNN)/src -ffunction-sections -fdata-sections -MMD -MP

# Host: runtime CPU dispatch (a static ISA choice can include extensions
# this machine lacks).
$(NCNN_HOST):
	cmake -S $(NCNN) -B build/ncnn-host -G Ninja $(NCNN_OPTS) -DNCNN_RUNTIME_CPU=ON >/dev/null
	ninja -C build/ncnn-host >/dev/null

$(NCNN_CAMERA):
	cmake -S $(NCNN) -B build/ncnn-camera -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/tools/zig-mipsel.cmake \
		$(NCNN_OPTS) -DNCNN_RUNTIME_CPU=OFF >/dev/null
	ninja -C build/ncnn-camera >/dev/null

# The convolution kernels: speed over size, and no -ffast-math (it lets the
# compiler merge the separate sums the kernels are written around).
FASTCONV_FLAGS := -O3 -ffp-contract=fast -fno-math-errno -fno-trapping-math
build/host/src/fastconv.o build/camera/src/fastconv.o: CXXFLAGS_COMMON += $(FASTCONV_FLAGS)

build/host/%.o: %.cpp | $(NCNN_HOST)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CXXFLAGS_COMMON) -Ibuild/ncnn-host/src -c $< -o $@

build/camera/%.o: %.cpp | $(NCNN_CAMERA)
	@mkdir -p $(dir $@)
	$(CAMERA_CXX) $(CXXFLAGS_COMMON) -Ibuild/ncnn-camera/src -c $< -o $@

build/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(if $(filter src/% test/%,$<),$(CFLAGS_COMMON),$(DEP_CFLAGS)) -c $< -o $@

build/camera/%.o: %.c
	@mkdir -p $(dir $@)
	$(CAMERA_CC) $(if $(filter src/%,$<),$(CFLAGS_COMMON),$(DEP_CFLAGS)) -c $< -o $@

OBJ_NAMES := $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(SRC) $(DEP_SRC)))
HOST_OBJ := $(addprefix build/host/,$(OBJ_NAMES))
CAMERA_OBJ := $(addprefix build/camera/,$(OBJ_NAMES))
LIB_HOST_OBJ := $(filter-out build/host/src/main.o,$(HOST_OBJ))

all: build/uvcd

third_party/mbedtls third_party/cjson third_party/opus:
	./fetch-deps.sh

build/uvcd: $(HOST_OBJ) $(NCNN_HOST)
	$(HOST_CXX) -o $@ $^ -lpthread

camera: build/uvcd-mipsel

build/uvcd-mipsel: $(CAMERA_OBJ) $(NCNN_CAMERA)
	$(CAMERA_CXX) -static -s -Wl,--gc-sections -o $@ $^ -lpthread -lm
	@ls -l $@

build/test_flv: build/host/test/test_flv.o $(LIB_HOST_OBJ) $(NCNN_HOST)
	$(HOST_CXX) -o $@ $^ -lpthread

build/test_motion: test/test_motion.c build/host/src/motion.o
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^

build/test_smart: test/test_smart.c build/host/src/smart.o build/host/$(CJSON)/cJSON.o
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^

build/test_stream_reply: test/test_stream_reply.c src/stream_reply.c src/stream_reply.h
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $(filter %.c,$^)

build/test_flv_timeline: test/test_flv_timeline.c build/host/src/flv.o build/host/src/h264.o \
		build/host/src/util.o build/host/$(MBEDTLS)/library/sha1.o \
		build/host/$(MBEDTLS)/library/platform_util.o
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^ -lpthread

# On the camera, with our kernel (test/test_capture_v4l2.c)
build/test_capture_v4l2-mipsel: test/test_capture_v4l2.c build/camera/src/capture_v4l2.o \
		build/camera/src/source.o build/camera/src/util.o build/camera/src/h264.o \
		build/camera/$(MBEDTLS)/library/sha1.o build/camera/$(MBEDTLS)/library/platform_util.o
	$(CAMERA_CC) $(CFLAGS_COMMON) -static -s -Wl,--gc-sections -o $@ $^ -lpthread -lm

build/test_opus-mipsel: test/test_opus.c $(addprefix build/camera/,$(OPUS_SRC:.c=.o))
	$(CAMERA_CC) $(CFLAGS_COMMON) -static -s -Wl,--gc-sections -o $@ $^ -lm
build/test_opus: test/test_opus.c $(addprefix build/host/,$(OPUS_SRC:.c=.o))
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^ -lm

# TLS handshake cost against Protect, on the camera (test/test_tls.c)
build/test_tls-mipsel: test/test_tls.c build/camera/src/tls.o build/camera/src/util.o \
		build/camera/src/camera_malloc.o \
		$(addprefix build/camera/,$(patsubst %.c,%.o,$(wildcard $(MBEDTLS)/library/*.c)))
	$(CAMERA_CC) $(CFLAGS_COMMON) -static -s -Wl,--gc-sections -o $@ $^ -lpthread -lm

build/test_audio-mipsel: test/test_audio.c build/camera/src/audio.o build/camera/src/util.o \
		build/camera/$(MBEDTLS)/library/sha1.o build/camera/$(MBEDTLS)/library/platform_util.o \
		$(addprefix build/camera/,$(patsubst %.c,%.o,$(filter $(VOAAC)/%,$(DEP_SRC))))
	$(CAMERA_CC) $(CFLAGS_COMMON) -static -s -Wl,--gc-sections -o $@ $^ -lpthread -lm

build/test_fastconv: build/host/test/test_fastconv.o build/host/src/fastconv.o $(NCNN_HOST)
	$(HOST_CXX) -o $@ $^ -lpthread

# The same with the kernels under the address sanitizer: their group tails
# and row buffers index close to the ends of ncnn's blobs.
build/test_fastconv_asan: test/test_fastconv.cpp src/fastconv.cpp $(NCNN_HOST)
	$(HOST_CXX) -std=c++11 -g -O1 -fsanitize=address,undefined -fno-rtti -Isrc -Ibuild/ncnn-host/src \
		-I$(NCNN)/src -o $@ $^ -lpthread

test: build/test_flv build/test_motion build/test_smart build/test_stream_reply build/test_flv_timeline \
		build/test_fastconv build/test_fastconv_asan
	./build/test_fastconv third_party/models/yolo-fastest-1.1 2>&1 | grep -v "^overwrite built-in"
	./build/test_fastconv_asan third_party/models/yolo-fastest-1.1 2>&1 | grep -c "largest difference"
	./build/test_motion
	./build/test_smart
	./build/test_stream_reply
	./build/test_flv_timeline
	./test/run.sh

clean:
	rm -rf build

-include $(HOST_OBJ:.o=.d) $(CAMERA_OBJ:.o=.d)
-include $(wildcard build/host/test/*.d)

.PHONY: all camera test clean
