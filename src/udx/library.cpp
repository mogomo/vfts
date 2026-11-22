// Library metadata shown in v_monitor.user_libraries.
#include "Vertica.h"
#include "BuildInfo.h"
#include "../engine/version.h"

RegisterLibrary("Mo (github.com/mogomo)",
                __DATE__,
                vfts::LIBRARY_VERSION,
                VERTICA_BUILD_ID_Brand_Version,
                "https://github.com/mogomo/vfts",
                "vfts: full-text search (BM25, phrase, proximity, fuzzy) on Vertica tables",
                "",
                "");
