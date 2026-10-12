// Compatibility patches for races in the game's own code: windows that the console's scheduling
// (two hardware threads, priority-driven) rarely opens, but that this port's fully concurrent,
// host-scheduled threads can hit. Each hook adds only the missing synchronization and then runs
// the original guest function; the hooked addresses are listed in hooks.txt.
//
// None are established for Armored Core 4 yet. The ACFA port patched two MultiStream races
// (stream released between its end and the streaming thread's next tick; sound bank lookups
// during bank activation); AC4 uses the same middleware, so the same windows may exist here, at
// other addresses. A patch is added only with evidence from an AC4 run.
