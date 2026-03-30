#pragma once

#include <common/bk_include.h>
#include "audio_play.h"

#ifdef __cplusplus
extern "C"
{
#endif

    bk_err_t app_ws_start(void);
    bk_err_t app_ws_stop(void);

#ifdef __cplusplus
}
#endif
