#pragma once

#include <libhighscore.h>

#include "Net.h"

G_BEGIN_DECLS

#define MELONDS_TYPE_CORE (melonds_core_get_type())

G_DECLARE_FINAL_TYPE (melonDSCore, melonds_core, MELONDS, CORE, HsCore)

void melonds_core_log (HsLogLevel level, const char *message);
void melonds_core_power_off (void);

const char *melonds_core_get_save_path (void);
const char *melonds_core_get_cache_path (void);
gboolean melonds_core_get_mic_active (void);
gulong melonds_core_get_microseconds (void);
melonDS::Net *melonds_core_get_net (void);

G_MODULE_EXPORT GType hs_get_core_type (void);

G_END_DECLS
