// M50SingleClap.h
// The three entry functions clap-wrapper links into every format.

#pragma once

bool tbm50s_entry_init (const char* pluginPath);
void tbm50s_entry_deinit();
const void* tbm50s_entry_get_factory (const char* factoryId);
