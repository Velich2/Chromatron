#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include <AE_General.r>
#endif

resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            "Chromatron"
        },
        Category {
            "Chromatron"
        },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
        CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
        CodeWinARM64 {"EffectMain"},
    #endif
#else
    #if defined(AE_OS_MAC)
        CodeMacIntel64 {"EffectMain"},
        CodeMacARM64 {"EffectMain"},
    #endif
#endif
        AE_PiPL_Version {
            2,
            0
        },
        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },
        /* PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 1) - must match GlobalSetup */
        AE_Effect_Version {
            524289
        },
        AE_Effect_Info_Flags {
            0
        },
        /* DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI */
        AE_Effect_Global_OutFlags {
            0x06000000
        },
        /* SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE | SUPPORTS_GPU_RENDER_F32 | SUPPORTS_THREADED_RENDERING */
        AE_Effect_Global_OutFlags_2 {
            0x0A001400
        },
        AE_Effect_Match_Name {
            "CHRM Chromatron"
        },
        AE_Reserved_Info {
            0
        },
        AE_Effect_Support_URL {
            "https://github.com/velich2/chromatron"
        }
    }
};
