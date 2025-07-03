#ifndef __userstatedoth__
#define __userstatedoth__

#include <crankshaft/tempbuff.h>
#include <crankshaft/mutex.h>
#include <crankshaft/list.h>
#include <crankshaft/alloc.h>
#include <crankshaft/slaballoc.h>
#include <crankshaft/socket.h>
#include <crankshaft/uuid.h>
#include <crankshaft/websocket.h>

#include "webmud.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BackscrollLine {
    const char *head;
    int size;
};

struct Backscroll {
    struct CS_TempBuffer *backscrollBuffer;
    struct CS_SlabAllocator *backscrollSlabs;
    struct CS_List *backscrollLines;
    const char *currentStartOfLine;
    int currentLineLength;
    int numLines;
    int numLinesPushed;
    struct CS_Mutex *backscrollMutex;
};

struct Backscroll *CreateBackscroll( int size, int numLines );
void DestroyBackscroll( struct Backscroll *backscroll );
void FeedBackscroll( struct Backscroll *backscroll, const char *input, int inputLength );

struct UserState;

#define MUD_NAME_MAX 64
#define MUD_ADDRESS_MAX 64
struct MudState {
    char name[MUD_NAME_MAX];
    char address[MUD_ADDRESS_MAX];
    int port;
    bool wantSSL;
    bool tlsV1;
    bool disconnected;
    bool running;
    int  linesWaiting;
    struct Backscroll *backscroll;
    struct CS_Socket *mudSocket;
    struct UserState *user;
};

struct MudState *CreateMud( struct UserState *user, char *name, char *address, int port, bool ssl, bool tlsV1, int size, int numlines );
bool ConnectMud( struct MudState *state );
void DestroyMud( struct MudState *mud );
bool DisconnectMud( struct MudState *mud );
void MudBackscrollToWebsockets( struct MudState *mud, struct CS_WebSocket *only, int number, char *filter, bool lock, bool lockUser );

#define SESSION_NAME_SIZE 64
struct UserState {
    const struct CS_ListItem *front;
    struct CS_List *muds;
    struct CS_List *websockets;
    char session[SESSION_NAME_SIZE];
    time_t lastLogin;
    time_t thisLogin;
    time_t lastInput;
    bool loopback;
    struct CS_Mutex *mutex;
};

struct UserState *CreateUserState( const char *sessionId );
void DestroyUserState( struct UserState *userState );
void TextToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, int length, bool lockUser );
void BinToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, int length, bool lockUser );
void NullStringToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, bool lockUser );
bool AddWebsocket( struct UserState *userState, struct CS_WebSocket *ws );
bool RemoveWebsocket( struct UserState *userState, struct CS_WebSocket *ws );
bool AddMud( struct UserState *userState, struct MudState *mud );
bool RemoveMud( struct UserState *userState, struct MudState *mud );
bool TextToFront( struct UserState *userState, const char *what, int length, bool lock );
bool NextConnection( struct UserState *userState );
bool LastConnection( struct UserState *userState );
bool PickConnection( struct UserState *userState, int index );
bool DisconnectFront( struct UserState *userState );
bool DeleteFront( struct UserState *userService );
struct MudState *MudStateByName( struct UserState *userState, const char *name );
bool PutMudFront( struct UserState *userState, struct MudState *mudState );
bool DisconnectOthers( struct UserState *userState, struct CS_WebSocket *ws );
bool SendStatus( struct UserState *userState, bool lockUserState );


#ifdef __cplusplus
}
#endif
#endif
