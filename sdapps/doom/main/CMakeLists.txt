# DOOM (#525): the engine is third_party/doomgeneric at the pinned commit (GPL-2.0-or-later, sha1.c GPL-3.0-or-later), the
# glue is ours. The sources are the Makefile's SRC_DOOM without any doomgeneric_<backend>, i_sdl*, i_allegro*, gusconf and
# icon: our backend is doomgeneric_lichen.c. The engine reaches libc only through doom_shim.h (force-included), so the ELF
# links against the OS's export table and nothing else.
set(DG "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/doomgeneric/doomgeneric")
set(ENGINE
    am_map.c
    d_event.c
    d_items.c
    d_iwad.c
    d_loop.c
    d_main.c
    d_mode.c
    d_net.c
    doomdef.c
    doomgeneric.c
    doomstat.c
    dstrings.c
    dummy.c
    f_finale.c
    f_wipe.c
    g_game.c
    hu_lib.c
    hu_stuff.c
    i_cdmus.c
    i_endoom.c
    i_input.c
    i_joystick.c
    i_scale.c
    i_sound.c
    i_system.c
    i_timer.c
    i_video.c
    info.c
    m_argv.c
    m_bbox.c
    m_cheat.c
    m_config.c
    m_controls.c
    m_fixed.c
    m_menu.c
    m_misc.c
    m_random.c
    memio.c
    p_ceilng.c
    p_doors.c
    p_enemy.c
    p_floor.c
    p_inter.c
    p_lights.c
    p_map.c
    p_maputl.c
    p_mobj.c
    p_plats.c
    p_pspr.c
    p_saveg.c
    p_setup.c
    p_sight.c
    p_spec.c
    p_switch.c
    p_telept.c
    p_tick.c
    p_user.c
    r_bsp.c
    r_data.c
    r_draw.c
    r_main.c
    r_plane.c
    r_segs.c
    r_sky.c
    r_things.c
    s_sound.c
    sha1.c
    sounds.c
    st_lib.c
    st_stuff.c
    statdump.c
    tables.c
    v_video.c
    w_checksum.c
    w_file.c
    w_file_stdc.c
    w_main.c
    w_wad.c
    wi_stuff.c
    z_zone.c)
list(TRANSFORM ENGINE PREPEND "${DG}/")

idf_component_register(SRCS doom_app.c doomgeneric_lichen.c ${ENGINE} INCLUDE_DIRS "${DG}")

# 8-bit frame plus palette (CMAP256), 320x200 (the zone and the 256 KB screen buffer are in PSRAM, through the OS's malloc).
# -w: it is third-party code, its warnings are not ours.
set(ENGINE_FLAGS -w -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -Dmalloc=lichen_doom_malloc -Dcalloc=lichen_doom_calloc -Drealloc=lichen_doom_realloc
    -Dfree=lichen_doom_free -Dstrdup=lichen_doom_strdup -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE
    -include "${CMAKE_CURRENT_LIST_DIR}/doom_shim.h")
target_compile_options(${COMPONENT_LIB} PRIVATE ${ENGINE_FLAGS} -Os)
# the quit entry of the engine is ours (it must end the app): i_system.c's I_Quit becomes lichen_doom_quit_engine
set_source_files_properties("${DG}/i_system.c" PROPERTIES COMPILE_DEFINITIONS "I_Quit=lichen_doom_quit_engine")
