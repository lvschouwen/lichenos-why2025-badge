#pragma once

// "fw psram sweep [passes [phase]]" (#86): tries every MSPI PSRAM DQS phase x delayline setting (31) at the running clock on
// pages we hold (known-bad ones and a few good ones) and prints bad bytes per setting. Diagnostic only: pins nothing; the
// original setting is back before anything else runs.
void psram_sweep_cmd(char const *args);
