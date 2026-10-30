// cass - Captain America: Super Soldier, ReXGlue recompiled project

#include "generated/default/cass_init.h"

#include "cass_app.h"

#include <float.h>

// Called from the [[midasm_hook]] entries in hints/original.toml.
void MaskFPUExceptions() {
  _clearfp();
  _controlfp_s(nullptr, _MCW_EM, _MCW_EM);
}

REX_DEFINE_APP(cass, CassApp::Create)
