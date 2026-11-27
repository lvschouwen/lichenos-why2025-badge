# lichen_ble (#598): L2 driver for the NimBLE host over esp_hosted's HCI link. The source is empty until B1 of #73.
idf_component_register(SRCS "lichen_ble.c"
                       PRIV_REQUIRES bt esp_hosted)

# Nothing calls NimBLE yet, so --gc-sections would drop all but what nimble_port_init reaches and the size gate would
# be baselined on a fraction of the stack (B1/B2 would trip it again). These roots keep linked what plan v3 uses: the
# lifecycle and the esp_hosted bridge (B1), GAP for all four roles, the security manager, the NVS bond store, dynamic
# GATT services with notify (B2) and the GATT client (C1). They only add code: nothing calls them at boot.
set(lichen_ble_roots
    # lifecycle (B1)
    esp_hosted_bt_host_stack_setup esp_hosted_bt_host_stack_teardown
    nimble_port_init nimble_port_deinit nimble_port_run nimble_port_stop
    nimble_port_freertos_init nimble_port_freertos_deinit
    ble_hs_id_infer_auto ble_store_config_init ble_store_util_delete_peer
    # GAP: broadcaster, peripheral, observer, central; pairing
    ble_svc_gap_init ble_svc_gap_device_name_set
    ble_gap_adv_set_fields ble_gap_adv_rsp_set_fields ble_gap_adv_start ble_gap_adv_stop
    ble_gap_disc ble_gap_disc_cancel ble_gap_connect ble_gap_terminate
    ble_gap_security_initiate ble_sm_inject_io
    # GATT server with services added and removed at runtime (B2)
    ble_svc_gatt_init ble_svc_gatt_changed
    ble_gatts_add_dynamic_svcs ble_gatts_delete_svc ble_gatts_notify_custom ble_gatts_indicate_custom
    # GATT client (C1)
    ble_gattc_exchange_mtu ble_gattc_disc_all_svcs ble_gattc_disc_all_chrs ble_gattc_disc_all_dscs
    ble_gattc_read ble_gattc_write_flat ble_gattc_write_no_rsp_flat
    # L2CAP connection-oriented channels (first user: the peer session, P1)
    ble_l2cap_create_server ble_l2cap_connect ble_l2cap_send ble_l2cap_recv_ready ble_l2cap_disconnect)
foreach(sym ${lichen_ble_roots})
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=${sym}")
endforeach()
