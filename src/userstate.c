#include <stdbool.h>
#include <pthread.h>

#include <crankshaft/alloc.h>
#include <crankshaft/mutex.h>
#include <crankshaft/slaballoc.h>
#include <crankshaft/logger.h>
#include <crankshaft/mutex.h>
#include <crankshaft/tempbuff.h>
#include <crankshaft/json.h>
#include <crankshaft/util.h>
#include "userstate.h"

static struct CS_SlabAllocator *backscrolls = NULL;
static struct CS_SlabAllocator *users = NULL;
static struct CS_SlabAllocator *muds = NULL;

pthread_mutex_t userstatemutex = PTHREAD_MUTEX_INITIALIZER;

struct Backscroll *privateGetBackscroll() {
    if( !backscrolls ) {
        pthread_mutex_lock( &userstatemutex );
        if( !backscrolls ) {
            backscrolls = CS_slabInit( "BACKSCROLL ITEMS", sizeof(struct Backscroll), 34, sizeof(void*) );
        }
        pthread_mutex_unlock( &userstatemutex );
    }
    if( !backscrolls ) {
        return NULL;
    }
    return CS_slabTakeZero( backscrolls );
}

void privateReturnBackscroll( struct Backscroll *bs ) {
    CS_slabReturn( backscrolls, bs );
}

struct UserState *privateGetUserState() {
    if( !users ) {
        pthread_mutex_lock( &userstatemutex );
        if( !users ) {
            users = CS_slabInit("USERS ITEMS", sizeof(struct UserState), 34, sizeof(void*) );
        }
        pthread_mutex_unlock( &userstatemutex );
    }
    if( !users ) {
        return NULL;
    }
    return CS_slabTakeZero( users );
}

void privateReturnUserState( struct UserState *state ) {
    CS_slabReturn( users, state );
}

struct MudState *privateGetMudState() {
    if( !muds ) {
        pthread_mutex_lock( &userstatemutex );
        if( !muds ) {
            muds = CS_slabInit("MUDS ITEMS", sizeof(struct MudState), 34, sizeof(void*) );
        }
        pthread_mutex_unlock( &userstatemutex );
    }
    return CS_slabTakeZero( muds );
}

void privateReturnMudState( struct MudState *state ) {
    CS_slabReturn( muds, state );
}

struct Backscroll *CreateBackscroll( int size, int numLines ) {
    struct Backscroll *returnValue = privateGetBackscroll();
    if( !returnValue ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->numLines = numLines;
    returnValue->backscrollLines = CS_listCreate( numLines );
    if( returnValue->backscrollLines == NULL ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->backscrollSlabs = CS_slabInit( "BACKSCROLL", sizeof( struct BackscrollLine ), numLines, sizeof( void * ) );
    if( returnValue->backscrollSlabs == NULL ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->backscrollBuffer = CS_tempAllocManual("BACKSCROLL", size);
    if( returnValue->backscrollBuffer == NULL ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->backscrollMutex = CS_mutexTakeNamed("BACKSCROLL MUTEX");
    if( returnValue->backscrollMutex == NULL ) goto CREATE_BACKSCROLL_ERROR;

    return returnValue;
CREATE_BACKSCROLL_ERROR:
    DestroyBackscroll( returnValue );
    return NULL;
}

void DestroyBackscroll( struct Backscroll *backscroll ) {
    if( backscroll == NULL ) return;
    if( backscroll->backscrollLines ) CS_listDestroy( backscroll->backscrollLines );
    if( backscroll->backscrollSlabs ) CS_slabFree( backscroll->backscrollSlabs );
    if( backscroll->backscrollBuffer ) CS_tempFreeManual( backscroll->backscrollBuffer );
    if( backscroll->backscrollMutex ) CS_mutexReturn( backscroll->backscrollMutex );
    memset( backscroll, 0, sizeof(struct Backscroll) );
    privateReturnBackscroll( backscroll );
}

#define LF '\n'

static bool privateAddLine( struct Backscroll *backscroll, const char *line, int lineLength ) {
    struct BackscrollLine *newLine = CS_slabTake( backscroll->backscrollSlabs );
    const char *endOfCopy = line + lineLength;
    if( newLine == NULL ) return true;
    newLine->head = line;
    newLine->size = lineLength;
    if( CS_listPushTail( backscroll->backscrollLines, newLine, 0 ) ) return true;
    while( CS_listCount( backscroll->backscrollLines ) > backscroll->numLines )
        CS_listRemove( backscroll->backscrollLines, CS_listGetHead( backscroll->backscrollLines ) );
    const struct CS_ListItem *cutMeAndPreviousOff = NULL;
    CS_LIST_ITER_REVERSE( backscroll->backscrollLines, listItem ) {
        if( listItem->next == NULL ) continue;
        struct BackscrollLine *checkLine = (struct BackscrollLine *)listItem->what;
        const char *startOfCheckline = checkLine->head;
        const char *endOfCheckline = startOfCheckline + checkLine->size;
        if( startOfCheckline < endOfCopy && endOfCheckline > line ) {
            cutMeAndPreviousOff = listItem;
            break;
        }
    }
    if( cutMeAndPreviousOff != NULL ) {
        const struct CS_ListItem * poppedItem;
        do {
            poppedItem = CS_listPopHead( backscroll->backscrollLines );
            CS_listReturnItem( backscroll->backscrollLines, poppedItem );
        } while( poppedItem != cutMeAndPreviousOff );
    }
    ++backscroll->numLinesPushed;
    return false;
}

static bool AddLine( struct Backscroll *backscroll, const char *line, int lineLength ) {
    char *copyTo = CS_tempGetManual( backscroll->backscrollBuffer, lineLength, 1 );
    char *endOfCopy = copyTo + lineLength;
    memcpy( copyTo, line, lineLength );
    //We've wrapped... stuff what was remaining
    if( copyTo < backscroll->currentStartOfLine ) {
        privateAddLine( backscroll, backscroll->currentStartOfLine, backscroll->currentLineLength );
        backscroll->currentStartOfLine = NULL;
    }
    const char *realStart = copyTo;
    int realLineLength = lineLength;
    if( backscroll->currentStartOfLine ) {
        realStart = backscroll->currentStartOfLine;
        realLineLength += backscroll->currentLineLength;
        backscroll->currentStartOfLine = NULL;
    }
    privateAddLine( backscroll, realStart, realLineLength );
    return false;
}

void FeedBackscroll( struct Backscroll *backscroll, const char *input, int inputLength ) {
    const char *current = input;
    const char *startOfInput = input;
    int currentStartIndex = 0;
    CS_mutexLock( backscroll->backscrollMutex );
    for( int i = 0; i < inputLength; ++i ) {
        if( !startOfInput ) {
            startOfInput = current;
            currentStartIndex = i;
        }
        if( *current == LF ) {
            AddLine( backscroll, startOfInput, i - currentStartIndex + 1 );
            startOfInput = NULL;
        }
        ++current;
    }
    if( startOfInput != NULL ) {
        backscroll->currentStartOfLine = startOfInput;
        backscroll->currentLineLength = current - startOfInput;
    } else {
        backscroll->currentStartOfLine = NULL;
        backscroll->currentLineLength = 0;
    }
    CS_mutexUnlock( backscroll->backscrollMutex );
}

struct MudState *CreateMud( struct UserState *user, char *name, char *address, int port, bool ssl, bool tlsV1, int size, int numlines ) {
    struct MudState *returnValue = privateGetMudState();

    if( returnValue ) {
        strncpy( returnValue->name, name, MUD_NAME_MAX - 1 );
        strncpy( returnValue->address, address, MUD_ADDRESS_MAX - 1 );
        returnValue->port = port;
        returnValue->wantSSL = ssl;
        returnValue->tlsV1 = tlsV1;
        returnValue->backscroll = CreateBackscroll( size, numlines );
        if( returnValue->backscroll == NULL ) {
            privateReturnMudState( returnValue );
            return NULL;
        }
        returnValue->user = user;
        returnValue->socketMutex = CS_mutexTake();
        if( returnValue->socketMutex == NULL ) {
            privateReturnMudState( returnValue );
            return NULL;
        }
        returnValue->keepaliveCommand = NULL;
        returnValue->keepaliveTime = 0;
        returnValue->keepaliveLast = 0;
    }

    return returnValue;
}

void DestroyMud( struct MudState *mud ) {
    if( mud ) {
        CS_mutexLock( mud->socketMutex );
        if( mud->running ) {
            CS_mutexUnlock( mud->socketMutex );
            mud->user = NULL;
            DisconnectMud( mud, true );
        } else {
            CS_mutexUnlock( mud->socketMutex );
            if( mud->backscroll ) DestroyBackscroll( mud->backscroll );
            mud->backscroll = NULL;
            if( mud->socketMutex ) CS_mutexReturn( mud->socketMutex );
            mud->socketMutex = NULL;
            if( mud->keepaliveCommand ) CS_cstringFree( mud->keepaliveCommand );
            mud->keepaliveCommand = NULL;
        }
    }
}

void *consumeThread(void *var) {
    struct MudState *mud = (struct MudState *)var;
    mud->running = true;
    while( true ) {
        //This socket has no mutexes on input or output.
        //We are the only ones who can delete the socket
        if( mud->mudSocket ) {
            struct CS_Socket *mudSocket = mud->mudSocket;
            int numBytesRead = CS_socketFillIncomingBuffer( mudSocket, false );
            
            if( numBytesRead < 0 ) {
                if( mud->mudSocket ) CS_socketDestroy( mud->mudSocket );
                mud->mudSocket = NULL;
                mud->disconnected = true;
                break;
            }
            if( mud->mudSocket && numBytesRead > 0 ) {
                struct CS_PushPullBuffer *pp = CS_socketLockInputBuffer( mud->mudSocket );
                FeedBackscroll( mud->backscroll, CS_PP_startOfData( pp ), CS_PP_dataSize( pp ) );
                CS_PP_reset( pp );
                CS_socketUnlockInputBuffer( mud->mudSocket );
                if( mud->user->front && mud->user->front->what == mud ) {
                    MudBackscrollToWebsockets( mud, NULL, 0, NULL, true, true );
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if( mud->user ) SendStatus(mud->user, true);
    if( mud->mudSocket ) CS_socketDestroy( mud->mudSocket );
    mud->mudSocket = NULL;
    mud->running = false;
    //If we've been disconnected from the user...kill ourselves.
    if( mud->user == NULL ) {
        DestroyMud( mud );
    }
    pthread_exit(NULL);
    return NULL;
}

bool ConnectMud( struct MudState *mud, bool allowNonRoutable ) {
    if( !mud ) return true;
    CS_mutexLock( mud->socketMutex );
    if( mud->mudSocket != NULL || mud->running ) {
        CS_mutexUnlock( mud->socketMutex );
        return true;
    }
    CS_mutexUnlock( mud->socketMutex );
    mud->mudSocket = CS_socketConnect( mud->address, !allowNonRoutable, mud->port, mud->wantSSL, mud->tlsV1, 8192, 8192, false, false );
    if( !mud->mudSocket ) {
        return true;
    }
    pthread_t newThread;
    int result = pthread_create(&newThread, NULL, consumeThread, mud);
    if( result < 0 ) {
        CS_socketDestroy( mud->mudSocket );
        mud->mudSocket = NULL;
        return true;
    }
    pthread_detach( newThread );
    return false;
}

bool DisconnectMud( struct MudState *mud, bool lock ) {
    if( !mud ) return true;
    if( lock ) CS_mutexLock( mud->socketMutex );
    if( mud->mudSocket == NULL || !mud->running ) {
        if( lock ) CS_mutexUnlock( mud->socketMutex );
        return true;
    }
    CS_socketClose( mud->mudSocket );
    mud->mudSocket = NULL;
    CS_mutexUnlock( mud->socketMutex );
    return false;
}

struct UserState *CreateUserState( const char *sessionId ) {
    struct UserState *returnValue = privateGetUserState();
    if( returnValue ) {
        strncpy( returnValue->session, sessionId, SESSION_NAME_SIZE - 1 );
        returnValue->muds = CS_listCreate( 8 );
        if( returnValue->muds == NULL ) goto USER_STATE_ERROR;
        returnValue->websockets = CS_listCreate( 8 );
        if( returnValue->websockets == NULL ) goto USER_STATE_ERROR;
        returnValue->lastLogin = 0;
        returnValue->thisLogin = returnValue->lastLogin = time(NULL);
        returnValue->mutex = CS_mutexTakeNamed("USER STATE");
        if( returnValue->mutex == NULL ) goto USER_STATE_ERROR;
        returnValue->jsonForOutput = CS_jsonNodeNew( 8192 );
        returnValue->sbForOutput = CS_SB_create( 8192 );
        if( returnValue->jsonForOutput == NULL ) goto USER_STATE_ERROR;
    }
    
    return returnValue;
USER_STATE_ERROR:
    DestroyUserState( returnValue );
    return NULL;
}

void DestroyUserState( struct UserState *userState ) {
    if( userState ) {
        if( userState->muds ) {
            CS_LIST_ITER( userState->muds, listItem ) {
                struct MudState *mud = (struct MudState *)listItem->what;
                DestroyMud( mud );
            }
        }
        if( userState->websockets ) {
            CS_LIST_ITER( userState->websockets, listItem ) {
                struct CS_WebSocket *ws = (struct CS_WebSocket *)listItem->what;
                CS_WS_close( ws, CS_WS_CLOSE_GOING_AWAY );
            }
            CS_listDestroy( userState->websockets );
        }
        if( userState->muds ) CS_listDestroy( userState->muds );
        if( userState->mutex ) CS_mutexReturn( userState->mutex );
        privateReturnUserState( userState );
        if( userState->jsonForOutput ) CS_jsonFree( userState->jsonForOutput );
        if( userState->sbForOutput ) CS_SB_free( userState->sbForOutput );
    }
}

bool match( const char *filter, const char *haystack, int length ) {
    if( filter == NULL ) return true;
    int filterLen = strlen(filter);
    for( int i = 0; i < length - filterLen; ++i ) {
        if( strncmp( filter, haystack + i, filterLen ) == 0 )
            return true;
    }
    return false;
}

void MudBackscrollToWebsockets( struct MudState *mud, struct CS_WebSocket *only, int number, char *filter, bool lock, bool lockUser ) {
    struct CS_Mutex *mutex = mud?mud->backscroll?mud->backscroll->backscrollMutex:NULL:NULL;
    if( mutex ) {
        if( lock ) CS_mutexLock(mutex);
        if( mud->backscroll->numLinesPushed > 0 || number != 0) {
            int currentWaiting = number!=0?-number:-mud->backscroll->numLinesPushed;
            if( number == 0 ) mud->backscroll->numLinesPushed = 0;
            const struct CS_ListItem *backscrollItem = CS_listGetByIndex( mud->backscroll->backscrollLines, currentWaiting );
            if( backscrollItem == NULL ) backscrollItem = CS_listGetHead( mud->backscroll->backscrollLines );
            if( backscrollItem ) {
                mud->linesWaiting = 0;
            }
            while( backscrollItem ) {
                struct BackscrollLine *current = (struct BackscrollLine *)backscrollItem->what;
                if( match( filter, current->head, current->size ) ) {
                    TextToWebsockets( mud->user, only, current->head, current->size, lockUser );
                }
                backscrollItem = backscrollItem->next;
            }
        }
        if( lock ) CS_mutexUnlock(mutex);
    }
}

static void EverythingToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, int length, bool lockUser, const char *name ) {
    if( !userState || !what || length == 0 ) return;
    if( lockUser ) CS_mutexLock( userState->mutex );
    struct CS_JsonNode *overall = CS_jsonNodeReset( userState->jsonForOutput );
    struct CS_JsonNode *container = CS_jsonNodeAppendObject( overall, NULL );
    struct CS_JsonNode *text = CS_jsonNodeAddUnquotedCstringWithLength(container, name, what, length );
    if( !text ) return;
    struct CS_StringBuilder *sb = userState->sbForOutput;
    CS_SB_reset( sb );
    CS_jsonNodePrintableToStringBuilder( overall, sb );
    //struct CS_StringBuilder *sb = CS_jsonNodePrintable( overall );
    if( !sb ) return;

    int currentSize = CS_SB_size( sb );
    int newSize = CS_align( currentSize, 4 );
    for( int i = currentSize; i < newSize; ++i ) {
        CS_SB_appendChar( sb, ' ' );
    }
    CS_SB_appendChar( sb, '\r' );
    CS_SB_appendChar( sb, '\n' );
    CS_SB_appendChar( sb, '\r' );
    CS_SB_appendChar( sb, '\n' );
    const struct CS_ListItem *toRemove = NULL;
    CS_LIST_ITER( userState->websockets, item ) {
        if( toRemove ) CS_listRemove( userState->websockets, toRemove );
        toRemove = NULL;

        struct CS_WebSocket *ws = (struct CS_WebSocket *)item->what;
        if( ws && (!only || ws == only) ) {
            struct CS_WebSocketFrame *returnFrame = CS_WS_createFrame( ws, CS_WS_OPCODE_BINARY, false, CS_SB_buffer(sb), CS_SB_size(sb) );
            if( CS_WS_pushFrame( ws, returnFrame, true) ) {
                CS_WS_close( ws, CS_WS_CLOSE_GOING_AWAY );
                toRemove = item;
            }
        }
    }
    if( toRemove ) CS_listRemove( userState->websockets, toRemove );
    if( lockUser ) CS_mutexUnlock( userState->mutex );
}
void BinToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, int length, bool lockUser ) {
    EverythingToWebsockets( userState, only, what, length, lockUser, "status" );
}

void TextToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, int length, bool lockUser ) {
    EverythingToWebsockets( userState, only, what, length, lockUser, "text" );
}

#define MAX_ACCEPTABLE_STRING 32767
void NullStringToWebsockets( struct UserState *userState, struct CS_WebSocket *only, const char *what, bool lockUser ) {
    int nLen = strnlen( what, MAX_ACCEPTABLE_STRING );
    if( nLen <= MAX_ACCEPTABLE_STRING ) {
        char * copyTo = CS_tempBuffZero( nLen + 4 );
        memcpy( copyTo, what, nLen );
        copyTo[nLen] = '\r';
        copyTo[nLen+1] = '\n';
        TextToWebsockets( userState, only, copyTo, nLen+2, lockUser );
    }
}
void NullStringToStatus( struct UserState *userState, struct CS_WebSocket *only, const char *what, bool lockUser ) {
    int nLen = strnlen( what, MAX_ACCEPTABLE_STRING );
    if( nLen <= MAX_ACCEPTABLE_STRING ) {
        char * copyTo = CS_tempBuffZero( nLen + 4 );
        memcpy( copyTo, what, nLen );
        copyTo[nLen] = '\r';
        copyTo[nLen+1] = '\n';
        userState->lastStatus = time(NULL);
        BinToWebsockets( userState, only, copyTo, nLen+2, lockUser );
    }
}

bool AddWebsocket( struct UserState *userState, struct CS_WebSocket *ws ) {
    if( !userState || !ws ) return true;
    bool returnValue = false;
    CS_mutexLock( userState->mutex );
    returnValue = CS_listPushTail( userState->websockets, ws, 0 );
    CS_mutexUnlock( userState->mutex );
    return returnValue;
}

bool RemoveWebsocket( struct UserState *userState, struct CS_WebSocket *ws ) {
    if( !userState || !ws ) return true;
    const struct CS_ListItem *removeMe = NULL;
    CS_mutexLock( userState->mutex );
    CS_LIST_ITER( userState->websockets, item ) {
        if( item->what == (void*)ws ) {
            removeMe = item;
            break;
        }
    }
    bool returnValue = true;
    if( removeMe ) {
        returnValue = CS_listRemove( userState->websockets, removeMe );
    }
    CS_mutexUnlock( userState->mutex );
    return returnValue;
}

static bool TextToMud( struct MudState *mud, const char *what, int length ) {
    if( !mud || !mud->mudSocket ) {
        return true;
    }
    struct CS_PushPullBuffer *pp = CS_socketLockOutputBuffer( mud->mudSocket );
    int bytesSent = 0;
    int bytesPushed = 0;
    const char *current = what;
    while( bytesSent < length ) {
        int bytesWrittenToBuffer = 0;
        int bytesWrittenToSocket = 0;
        if( bytesPushed < length ) {
            bytesWrittenToBuffer =
                CS_PP_readFromBuffer( pp, current + bytesPushed, length - bytesPushed );
        }
        if( bytesWrittenToBuffer < 0 ) {
            break;
        }
        bytesPushed += bytesWrittenToBuffer;
        bytesWrittenToSocket = CS_socketEmptyOutputBuffer( mud->mudSocket, false );
        if( bytesWrittenToSocket < 0 ) {
            break;
        }
        bytesSent += bytesWrittenToSocket;
    }
    CS_socketUnlockOutputBuffer( mud->mudSocket );
    if( bytesSent < length )
        return true;

    return false;
 }

bool TextToFront( struct UserState *userState, const char *what, int length, bool lock ) {
    if( !userState || !userState->mutex || !userState->front ) return true;
    if( lock ) CS_mutexLock( userState->mutex );
    struct MudState *mud = (struct MudState *)userState->front->what;
    bool returnValue = TextToMud(mud, what, length);
    if( lock ) CS_mutexUnlock( userState->mutex );
    return returnValue;
}

bool AddMud( struct UserState *userState, struct MudState *mud ) {
    if( !userState || !mud ) return true;
    CS_mutexLock( userState->mutex );

    CS_listPushTail( userState->muds, mud, 0 );
    userState->front = CS_listGetTail( userState->muds );
    SendStatus( userState, false );

    CS_mutexUnlock( userState->mutex );
    return false;
}

bool RemoveMud( struct UserState *userState, struct MudState *mud ) {
    if( userState == NULL || mud == NULL ) return true;
    const struct CS_ListItem *removeMe = NULL;
    CS_mutexLock( userState->mutex );
    CS_LIST_ITER( userState->muds, item ) {
        if( item->what == (void*)mud ) {
            removeMe = item;
            break;
        }
    }
    bool returnValue = true;
    if( removeMe ) {
        if( userState->front == removeMe ) {
            if( removeMe->next ) {
                userState->front = removeMe->next;
            } else {
                userState->front = removeMe->last;
            }
        }
        returnValue = CS_listRemove( userState->muds, removeMe );
    }
    CS_mutexUnlock( userState->mutex );
    return returnValue;
}
static void setNewFront( struct UserState *userState, const struct CS_ListItem *next ) {
    if( next != userState->front ) {
        userState->front = next;
        if( next ) {
            struct MudState *state = (struct MudState *)userState->front->what;
            NullStringToWebsockets( userState, NULL, CS_tempBuffSnprintf( 128, "Switched to %s.", state->name ), false );
            MudBackscrollToWebsockets( (struct MudState *) userState->front->what, NULL, 0, NULL, true, false );
            SendStatus( userState, false );
        } else {
            NullStringToWebsockets( userState, NULL, "Switched to NO WORLD.", false );
        }
    }
}
bool NextConnection( struct UserState *userState ) {
    CS_mutexLock( userState->mutex );

    const struct CS_ListItem *next = NULL;

    if( userState->front ) {
        if( userState->front->next ) {
            next = userState->front->next;
        } else {
            next = CS_listGetHead( userState->muds );
        }
    }

    setNewFront( userState, next );

    CS_mutexUnlock( userState->mutex );
    return false;
}
bool LastConnection( struct UserState *userState ) {
    CS_mutexLock( userState->mutex );

    const struct CS_ListItem *next = NULL;

    if( userState->front ) {
        if( userState->front->last ) {
            next = userState->front->last;
        } else {
            next = CS_listGetTail( userState->muds );
        }
    }

    setNewFront( userState, next );

    CS_mutexUnlock( userState->mutex );
    return false;
}

bool PickConnection( struct UserState *userState, int index ) {
    CS_mutexLock( userState->mutex );

    const struct CS_ListItem *next = CS_listGetByIndex( userState->muds, index );

    setNewFront( userState, next );

    CS_mutexUnlock( userState->mutex );
    return false;
}

struct MudState *MudStateByName( struct UserState *userState, const char *name ) {
    CS_mutexLock( userState->mutex );

    CS_LIST_ITER( userState->muds, item ) {
        struct MudState *mud = (struct MudState *)item->what;
        if( strncmp( name, mud->name, MUD_NAME_MAX ) == 0 ) {
            CS_mutexUnlock( userState->mutex );
            return mud;
        }
    }
    CS_mutexUnlock( userState->mutex );
    return NULL;
}

bool PutMudFront( struct UserState *userState, struct MudState *mudState ) {
    CS_mutexLock( userState->mutex );
    CS_LIST_ITER( userState->muds, item ) {
        if( mudState == item->what ) {
            setNewFront( userState, item );
            break;
        }
    }
    CS_mutexUnlock( userState->mutex );
    return false;
}


bool DisconnectFront( struct UserState *userState ) {
    if( userState == NULL ) return true;
    CS_mutexLock( userState->mutex );

    if( !userState->front || !userState->front->what ) {
        CS_mutexUnlock( userState->mutex );
        return true;
    }

    DisconnectMud( (struct MudState *)userState->front->what, true );

    CS_mutexUnlock( userState->mutex );
    return false;
}

bool DeleteFront( struct UserState *userState ) {
    if( userState == NULL ) return true;
    CS_mutexLock( userState->mutex );
    if( !userState->front || !userState->front->what ) {
        CS_mutexUnlock( userState->mutex );
        return true;
    }
    struct MudState *mud = (struct MudState *)userState->front->what;
    DisconnectMud( mud, false );
    mud->user = NULL;
    const struct CS_ListItem *next = userState->front->next;
    if( !next ) next = userState->front->last;
    CS_listRemove( userState->muds, userState->front );
    userState->front = next;
    CS_mutexUnlock( userState->mutex );
    return false;
}

bool DisconnectOthers( struct UserState *userState, struct CS_WebSocket *ws ) {
    CS_mutexLock( userState->mutex );
    CS_LIST_ITER( userState->websockets, item ) {
        struct CS_WebSocket *checkWs = (struct CS_WebSocket *)item->what;
        if( checkWs != ws ) {
            CS_WS_close( checkWs, CS_WS_CLOSE_NORMAL );
        }
    }
    CS_mutexUnlock( userState->mutex );
    return false;
}

bool SendStatus( struct UserState *userState, bool lockUserState ) {
    if( lockUserState ) CS_mutexLock( userState->mutex );
    time_t now = time(NULL);
    if( now > userState->lastStatus + 5 ) {
        userState->lastStatus = time(NULL);

        struct CS_StringBuilder *sb = userState->sbForOutput;
        CS_SB_reset(sb);
        const struct CS_ListItem *current = userState->front;
        if( current == NULL ) {
            CS_SB_append( sb, "NO MUDS" );
        } else {
            do {
                const struct MudState *mudState = current->what;
                CS_SB_append( sb, mudState->name );
                if( !mudState->mudSocket ) CS_SB_append(sb, "!");
                if( mudState->linesWaiting ) CS_SB_append(sb, "*");
                CS_SB_append(sb, " ");
                current = current->next;
                if( current == NULL ) current = CS_listGetHead( userState->muds );
            } while( current != userState->front );
        }
        BinToWebsockets( userState, NULL, CS_SB_buffer( sb ), CS_SB_size( sb ), false );
    }

    if( lockUserState ) CS_mutexUnlock( userState->mutex );
    return false;
}

bool DoKeepalives( struct UserState *userState, bool lockUserState ) {
    if( !userState || !userState->mutex || !userState->muds ) return true;
    if( lockUserState ) CS_mutexLock( userState->mutex );
    time_t now = time(NULL);
    if( userState->muds ) {
        CS_LIST_ITER( userState->muds, listItem ) {
            struct MudState *mud = (struct MudState *)listItem->what;
            if( mud->keepaliveCommand && (now > mud->keepaliveLast + mud->keepaliveTime) ) {
                mud->keepaliveLast = now;
                TextToMud( mud, mud->keepaliveCommand, strlen(mud->keepaliveCommand) );
            }
        }
    }
    if( lockUserState ) CS_mutexUnlock( userState->mutex );
    return false;
}

