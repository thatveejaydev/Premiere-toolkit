#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "PluginInfo.h"

resource 'PiPL' (16000) {
    {
        Kind { AEEffect },
        Name { BEVEL_NAME },
        Category { BEVEL_CATEGORY },
        CodeWin64X86 { "EffectMain" },
        AE_PiPL_Version { 2, 0 },
        AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
        AE_Effect_Version { BEVEL_VERSION },
        AE_Effect_Info_Flags { 0 },
        AE_Effect_Global_OutFlags { BEVEL_OUT_FLAGS },
        AE_Effect_Global_OutFlags_2 { BEVEL_OUT_FLAGS_2 },
        AE_Effect_Match_Name { BEVEL_MATCH_NAME },
        AE_Reserved_Info { 0 }
    }
};
