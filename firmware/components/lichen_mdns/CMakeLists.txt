# Lichen's mDNS name and update-server service (#156). mdns_name.c is pure (host-tested in host_tests/mdns_name_test.c).
idf_component_register(SRCS "mdns_name.c" "lichen_mdns.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_hw_support espressif__mdns)
