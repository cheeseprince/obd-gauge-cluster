#pragma once
// Software-watchdog kick for long synchronous core-0 OBD work.
//
// main.cpp's core-1 loop reboots the board if the OBD task's heartbeat stalls
// >240s. The heartbeat is normally stamped once per poll() — but the one-shot
// diagnostics ('x' scan = 544 sequential probes, 'g' probe = 20 samples) run
// entirely INSIDE a single poll(), and with a quiet ECU each probe waits its
// full timeout, easily exceeding the window. Those loops call this per probe
// so a healthy-but-slow diagnostic can't be mistaken for a hang. Defined in
// main.cpp (it owns the heartbeat); safe to call from core 0 at any rate.
void obdWatchdogKick();
