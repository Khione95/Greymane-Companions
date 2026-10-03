#pragma once

#define GREYMANE_VERSION "1.0"

// The game's mercenary clan, used on its server thread (the server's message
// loop is hooked; requests from the keys run there).

// Checks the game build and the code this needs; hooks the server loop.
bool GameInit();

// Requests, carried out on the server thread at its next message (Bring back:
// your companions come to you).
enum Request { REQUEST_NONE, REQUEST_RESCUE };
void GameRequest(Request r);
