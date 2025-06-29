#include <stdbool.h>
#include <pthread.h>

#include <crankshaft/alloc.h>
#include <crankshaft/mutex.h>
#include <crankshaft/slaballoc.h>
#include <crankshaft/logger.h>
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
}

static bool AddLine( struct Backscroll *backscroll, const char *line, int lineLength ) {
    char *copyTo = CS_tempGetManual( backscroll->backscrollBuffer, lineLength, 1 );
    char *endOfCopy = copyTo + lineLength;
    memcpy( copyTo, line, lineLength );
    //We've wrapped... stuff what was remaining
    if( copyTo < backscroll->currentStartOfLine ) {
        CS_LOG_TRACE("Backscroll wrap.");
        privateAddLine( backscroll, backscroll->currentStartOfLine, backscroll->currentLineLength );
        backscroll->currentStartOfLine = NULL;
    }
    const char *realStart = copyTo;
    int realLineLength = lineLength;
    if( backscroll->currentStartOfLine ) {
        CS_LOG_TRACE("Concat 'lines')");
        realStart = backscroll->currentStartOfLine;
        realLineLength += backscroll->currentLineLength;
        backscroll->currentStartOfLine = NULL;
    }
    CS_LOG_TRACE("Add Line.");
    privateAddLine( backscroll, realStart, realLineLength );
}

void FeedBackscroll( struct Backscroll *backscroll, const char *input, int inputLength ) {
    const char *current = input;
    const char *startOfInput = input;
    int currentStartIndex = 0;
    for( int i = 0; i < inputLength; ++i ) {
        if( !startOfInput ) startOfInput = current;
        if( *current == LF ) {
            AddLine( backscroll, startOfInput, i - currentStartIndex + 1 );
            startOfInput = NULL;
        }
        ++current;
    }
    if( startOfInput != NULL ) {
        CS_LOG_ERROR("trashleft");
        backscroll->currentStartOfLine = startOfInput;
        backscroll->currentLineLength = current - startOfInput;
    } else {
        CS_LOG_ERROR("notrash");
        backscroll->currentStartOfLine = NULL;
        backscroll->currentLineLength = 0;
    }
}

struct MudState *CreateMud( char *name, char *address, int port, int size, int numlines ) {
    return NULL;
}

struct MudState *DestroyMud( struct MudState *mud ) {
    return NULL;
}
