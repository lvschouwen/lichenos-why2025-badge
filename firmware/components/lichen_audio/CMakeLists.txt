# Notification sounds (#67). Task 1D: only the pure tone patterns, volume curve, gating, rate limit and sample generator
# (host_tests/audio_pattern_test.c). No driver and no hookup yet (the I2S driver and the merge into the single
# lichen_notify feedback registration with haptics come in 2C), so nothing here is linked into an image.
idf_component_register(SRCS "audio_pattern.c" INCLUDE_DIRS ".")
