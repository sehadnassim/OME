LOCAL_PATH := $(call get_local_path)
include $(DEFAULT_VARIABLES)

LOCAL_TARGET := orchestrator

$(call add_pkg_config,openssl)

include $(BUILD_STATIC_LIBRARY)

