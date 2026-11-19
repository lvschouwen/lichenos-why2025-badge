// The mesh lock for mesh_advtab_run.c (#469): mesh_int.h is C++ (mesh_link.h), so a C file cannot include it
#include "mesh_advtab_run.h"
#include "mesh_int.h"

extern "C" void mesh_advtab_run_lock(void) {
    mesh_int::mesh_lock();
}

extern "C" void mesh_advtab_run_unlock(void) {
    mesh_int::mesh_unlock();
}
