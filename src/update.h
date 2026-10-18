// Keeps a published copy of gear up to date by itself, as gsignal does. Every minute it reads a
// manifest from the update server: the build it describes and the name, SHA-256 and size of each
// file of that build, signed with the publisher's Ed25519 key. When the build is another and the
// signature holds, each file that differs is downloaded and checked against its hash, and once all
// have arrived each is put in place of the old one, which is renamed aside (a running program on
// Windows can be renamed but not overwritten). The window then starts the new build when it is idle.
//
// The server is reached over plain HTTP: the signature, made with a key that never leaves the
// publisher's machine, is what keeps a changed file out, whoever answers.
#pragma once
#include <stdbool.h>

typedef struct gear_update gear_update;

// The build this program is, as tools/publish-update named it, or NULL for a build of one's own,
// which never updates itself.
const char *gear_update_build(void);
// Starts looking for updates for the program in `folder` (ending in a separator), when this is a
// published build; NULL otherwise. GEAR_UPDATE_FROM names another server, for trying it out.
gear_update *gear_update_start(const char *folder);
bool gear_update_ready(gear_update *u);  // a newer build is in place, to be started
void gear_update_free(gear_update *u);   // stops looking; waits for a download under way to end

// Starts the program in `folder` again with these arguments, for the new build to take over.
bool gear_update_restart(const char *folder, int argc, char **argv);
