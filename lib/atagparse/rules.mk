LOCAL_DIR := $(GET_LOCAL_DIR)

INCLUDES += -I$(LOCAL_DIR)/include

MODULES += lib/libfdt

OBJS += \
	$(LOCAL_DIR)/atagparse.o \
	$(LOCAL_DIR)/cmdline.o
