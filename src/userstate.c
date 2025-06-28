#include <stdbool.h>
#include <pthread.h>

#include <crankshaft/alloc.h>
#include <crankshaft/mutex.h>
#include "userstate.h"

struct CS_SlabAllocator *backscrolls = NULL;

pthread_mutex_t backscrollsMutex = PTHREAD_MUTEX_INITIALIZER;

struct Backscroll *CreateBackscroll( int size, int numLines ) {
    struct Backscroll *returnValue = CS_allocZero( sizeof (struct Backscroll) );
    if( !returnValue ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->numLines = numLines;
    returnValue->backscrollLines = CS_listCreate( numLines );
    if( returnValue->backscrollLines == NULL ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->backscrollSlabs = CS_slabInit( "BACKSCROLL", sizeof( struct BackscrollLine ), numLines, sizeof( void * ) );
    if( returnValue->backscrollSlabs == NULL ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->backscrollBuffer = CS_tempAllocManual("BACKSCROLL", size);
    if( returnValue->backscrollBuffer == NULL ) goto CREATE_BACKSCROLL_ERROR;
    returnValue->backscrollMutex = CS_mutexGrab();
    if( returnValue->backscrollMutex == NULL ) goto CREATE_BACKSCROLL_ERROR;

    return returnValue;
CREATE_BACKSCROLL_ERROR:
    DestroyBackscroll( returnValue );
    return NULL;
}

void DestroyBackscroll( struct Backscroll *backscroll ) {
    if( backscroll == NULL ) return;
    CS_listDestroy( backscroll->backscrollLines );
    CS_slabFree( backscroll->backscrollSlabs );
    CS_tempFreeManual( backscroll->backscrollBuffer );
    CS_mutexReturn( backscroll->backscrollMutex );
    CS_free( backscroll );
}

void FeedBackscroll( struct Backscroll *backscroll, const char *input, int inputLength ) {
}

struct MudState *CreateMud( char *name, char *address, int port, int size, int numlines ) {
    return NULL;
}

struct MudState *DestroyMud( struct MudState *mud ) {
    return NULL;
}
