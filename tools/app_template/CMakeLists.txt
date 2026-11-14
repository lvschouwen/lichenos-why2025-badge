# @NAME_C@ (scaffolded by `tools/badge.sh new-app`; see docs/app-api.md "An app in 10 minutes"). An app component only
# PRIV_REQUIRES what the L6 profile of tools/layers.conf allows: lichen_api brings lvgl and every header an app may use.
idf_component_register(SRCS "app_@ID@.c" "@ID@_model.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lichen_api)
