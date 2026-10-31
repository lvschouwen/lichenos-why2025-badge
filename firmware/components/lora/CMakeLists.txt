# Protocol, config parser/supervisor and packet summaries are pure; lora_proto is shared with the C6
# (host-tested: lora_proto_test, lora_link_test); the MeshCore packet summary is the mesh component's (#84 T7); tomlc17
# reads through shared/why_io_libc.c
idf_component_register(SRCS "lora.c" "lora_tx.c" "../../../shared/lora/lora_proto.c" "lora_link.c"
                            "thirdparty/tomlc17.c"
                            "../../../shared/why_io_libc.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../../../shared/lora" "../../../shared"
                       PRIV_REQUIRES esp_hosted esp_timer fw_console)
# why_io.h uses clang's nullability qualifiers
target_compile_definitions(${COMPONENT_LIB} PRIVATE _Nullable=)
# why_io.h names struct stat without including its header
set_source_files_properties("thirdparty/tomlc17.c" "../../../shared/why_io_libc.c"
                            PROPERTIES COMPILE_OPTIONS "-include;sys/stat.h")
