#pragma once
#include <stdbool.h>

// Runs the official PokeMMO client (its `main`) inside the adapters of this project and waits for it. Returns when the client returned
// (its Exit button), when the system asked the application to close and the client did not finish in time, or when it could not start.
// True: the game ended normally or was closed. False: it did not start or it ended with an error (gameFailure() says why).
bool gameRun(const char *client_path);
// The client's own threads cannot be joined: when this is true the application must end the whole process (never return to hbloader).
bool gameThreadsLeft(void);
// Stops every thread of the client for good (they cannot be ended): called right before the application leaves the system.
void gamePauseThreads(void);
const char *gameFailure(void);  // a short sentence for the player, valid after gameRun returned false
