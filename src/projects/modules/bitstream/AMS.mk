LOCAL_PATH := $(call get_local_path)
include $(DEFAULT_VARIABLES)

LOCAL_TARGET := bitstream

LOCAL_SOURCE_FILES := $(LOCAL_SOURCE_FILES) \
    $(call get_sub_source_list,aac) \
	$(call get_sub_source_list,nalu) \
    $(call get_sub_source_list,h264) \
	$(call get_sub_source_list,h265)

LOCAL_HEADER_FILES := $(LOCAL_HEADER_FILES) \
    $(call get_sub_header_list,aac) \
	$(call get_sub_header_list,nalu) \
    $(call get_sub_header_list,h264) \
	$(call get_sub_header_list,h265)

$(call add_pkg_config,srt)

include $(BUILD_STATIC_LIBRARY)