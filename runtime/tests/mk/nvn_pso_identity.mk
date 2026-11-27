# native-test-nvn_pso_identity：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_pso_identity
ifeq ($(S2A_UNAME),Darwin)
# 管线归档身份要设备：设备没建好时不算身份、不删归档；连续两次起局归档被复用
$(BUILD)/tests/t_nvn_pso_identity: graphics/nvn/tests/t_nvn_pso_identity.m $(NVN_MTL_DEPS) $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -Wno-unused-function -Wno-unguarded-availability-new -Wno-unguarded-availability \
	  $(NATIVE_INC) -Igraphics/nvn/host -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/third_party/vulkan-headers/include \
	  -o $@ graphics/nvn/tests/t_nvn_pso_identity.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_pso_identity
native-test-nvn_pso_identity: $(BUILD)/tests/t_nvn_pso_identity
	$(BUILD)/tests/t_nvn_pso_identity
TEST_TARGETS += native-test-nvn_pso_identity
endif
