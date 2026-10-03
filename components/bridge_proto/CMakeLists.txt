# test_bridge_proto.c 也在 SRCS 里：开机自检，见 main/main.cpp 的 bridge_proto_selftest()
idf_component_register(SRCS "bridge_proto.c" "test_bridge_proto.c"
                    INCLUDE_DIRS "include")
