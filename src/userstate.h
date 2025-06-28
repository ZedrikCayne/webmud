#include <crankshaft/tempbuff.h>
#include <crankshaft/mutex.h>
#include <crankshaft/list.h>
#include <crankshaft/alloc.h>
#include <crankshaft/slaballoc.h>
#include <crankshaft/socket.h>
#include <crankshaft/uuid.h>

struct BackscrollLine {
    char *head;
    int size;
};

struct Backscroll {
    struct CS_TempBuffer *backscrollBuffer;
    struct CS_SlabAllocator *backscrollSlabs;
    struct CS_List *backscrollLines;
    char *currentStartOfLine;
    int numLines;
    pthread_mutex_t *backscrollMutex;
};

struct Backscroll *CreateBackscroll( int size, int numLines );
void DestroyBackscroll( struct Backscroll *backscroll );
void FeedBackscroll( struct Backscroll *backscroll, const char *input, int inputLength );

#define MUD_NAME_MAX 64
struct MudState {
    char name[MUD_NAME_MAX];
    struct Backscroll *backscroll;
    struct CS_Socket *mudSocket;
};

struct MudState *CreateMud( char *address, int port, int size, int numlines );
struct MudState *DestroyMud( struct MudState *mud );

#define SESSION_NAME_SIZE 64
struct UserState {
    int currentMudState;
    struct CS_List *mudStates;
    char session[SESSION_NAME_SIZE];
};

struct UserState *CreateUserState( const char *sessionId );
void DestroyUserState( struct UserState *userState );

