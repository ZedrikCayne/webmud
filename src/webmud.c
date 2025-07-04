#include <string.h>

#include <crankshaft/hashtable.h>
#include <crankshaft/server.h>
#include <crankshaft/socket.h>
#include <crankshaft/websocket.h>
#include <crankshaft/html.h>
#include <crankshaft/mime.h>
#include <crankshaft/googleservices.h>
#include <crankshaft/json.h>
#include <crankshaft/logger.h>
#include <crankshaft/list.h>
#include <crankshaft/util.h>
#include <crankshaft/storage.h>

#include "webmud.h"

static const struct CS_Storage *longTermStorage = NULL;
static struct CS_HashTable *cheapSessions = NULL;
static struct CS_HashTable *googleIdToSessionId = NULL;

static bool appAutoLogin;
static bool appAllowNonRoutable;

static char loremIpsum[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.";

bool startApplication(bool autoLogin, bool allowNonRoutable) {
    longTermStorage = CS_storageOpen( "USER_DB", "file=secrets/webmud_userdb.sqlite", CS_STORAGE_BACKEND_SQLITE );
    appAutoLogin = autoLogin;
    appAllowNonRoutable = allowNonRoutable;
    googleIdToSessionId = CS_HASHTABLE_STRING_VOID( 256, CS_HASHTABLE_FLAG_MUTEX|CS_HASHTABLE_FLAG_VERY_PEDANTIC);
    cheapSessions = CS_HASHTABLE_STRING_VOID( 256, CS_HASHTABLE_FLAG_MUTEX|CS_HASHTABLE_FLAG_VERY_PEDANTIC);
    return !googleIdToSessionId||!cheapSessions;
}

void killApplication() {
    if( googleIdToSessionId ) CS_hashtableFree( googleIdToSessionId );
    if( cheapSessions ) CS_hashtableFree( cheapSessions );
}

bool cookieFilter( struct CS_ClientInfo *info ) {
    const char *cookieValue = CS_serverGetRequestCookie( info, "session" );
    if( cookieValue != NULL ) {
        const void *currentSession = CS_hashtableGet( cheapSessions, cookieValue );
        if( currentSession != CS_HASHTABLE_ERROR && currentSession != NULL ) {
            return false;
        }
    }
    if( appAutoLogin ) autoLoginUtil(info);
    return loginPageReturn(info);
}

static struct CS_WebSocketFrame *backscrollToFrame( struct CS_WebSocket *ws,
        const struct CS_ListItem *item ) {
    struct BackscrollLine *line = (struct BackscrollLine *)item->what;
    struct CS_WebSocketFrame *returnFrame = CS_WS_createFrame( ws, CS_WS_OPCODE_TEXT, false, line->head, line->size);
    return returnFrame;
}

struct commandToHandler {
    char *command;
    int  commandLength;
    bool (*executeMe)(struct CS_WebSocket *ws, struct UserState *user,const char *line, int length);
};

static char *tempCopyWithNulls( const char *line, int length ) {
    char *copy = CS_tempBuffZero( length + 4 );
    if( copy == NULL ) {
        return NULL;
    }
    memcpy( copy, line, length );
    return copy;
}

static bool prevCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int length ) {
    LastConnection( userState );
    return false;
}
static bool nextCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int length ) {
    NextConnection( userState );
    return false;
}
static bool pickCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int length ) {
    char *temp = tempCopyWithNulls( line, length );
    char *savePtr;
    char *command = strtok_r( temp, " ", &savePtr );
    char *index = strtok_r( NULL, " ", &savePtr );
    int which = 0;
    if( index == NULL ) {
        return false;
    }
    PickConnection( userState, which );
    return false;
}

static bool infoCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int length ) {
    struct CS_StringBuilder *sb = CS_SB_create( 1024 );
    if( !sb ) return true;
    CS_mutexLock( userState->mutex );

    CS_SB_append( sb, "info:\r\n" );

    CS_SB_printf( sb, "Num Web Clients: %d\r\n", CS_listCount( userState->websockets ) );

    if( CS_listCount( userState->muds ) > 0 ) {
        int currentIndex = 1;
        CS_LIST_ITER( userState->muds, item ) {
            struct MudState *mud = (struct MudState *)item->what;
            if( mud ) {
                CS_SB_printf( sb, "Connection %d ", currentIndex );
                CS_SB_append( sb, mud->name );
                if( item == userState->front ) CS_SB_append(sb, " current");
                if( mud->linesWaiting ) CS_SB_printf( sb, " %d lines waiting", mud->linesWaiting );
                CS_SB_append(sb, "\r\n");
            }
            ++currentIndex;
        } 
    } else {
        CS_SB_append( sb, "no connections\r\n" );
    }
    
    TextToWebsockets( userState, ws, CS_SB_buffer( sb ), CS_SB_size( sb ), false );

    CS_mutexUnlock( userState->mutex );
    return false;
}

static bool connectCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int length ) {
    char *copy = tempCopyWithNulls( line, length );
    char *savePtr;
    char * command = strtok_r(copy, " ", &savePtr);
    char * name = strtok_r(NULL, " ", &savePtr);
    if( name == NULL ) {
        if( userState->front && userState->front->what ) {
            struct MudState *mud = (struct MudState*)userState->front->what;
            if( mud->disconnected || !mud->running ) {
                if( ConnectMud( mud, appAllowNonRoutable ) ) {
                    NullStringToWebsockets( userState, ws, "Failed to connect to remote.", true );
                    return true;
                }
            } else {
                NullStringToWebsockets( userState, ws, CS_tempBuffSnprintf( 128, "Still connected to %s.", mud->name), true );
                return true;
            }
        }
        NullStringToWebsockets( userState, ws, "Connection needs a name.", true );
        return true;
    }
    struct MudState *currentNamed = MudStateByName( userState, name );
    char * address = strtok_r(NULL, " ", &savePtr);
    if( address == NULL ) {
        if( currentNamed ) {
            PutMudFront( userState, currentNamed );
            if( currentNamed->disconnected || !currentNamed->running ) {
                if( ConnectMud( currentNamed, appAllowNonRoutable ) ) {
                    NullStringToWebsockets( userState, ws, "Could not connect to remote.", true );
                    return true;
                }
            }
            return false;
        }
        NullStringToWebsockets( userState, ws, "Need an address.", true );
        return true;
    } else {
        if( currentNamed ) {
            NullStringToWebsockets( userState, ws,
                   CS_tempBuffSnprintf( 128, "Already have a connection named %s", name ), true );
            return true;
        }
    }
    char * port_str = strtok_r(NULL, " ", &savePtr);
    if( port_str == NULL ) {
        NullStringToWebsockets( userState, ws, "Need a port number.", true );
        return true;
    }
    long portNum = strtol( port_str, NULL, 10 );
    if( portNum < 1024 ) {
        NullStringToWebsockets( userState, ws, "We don't allow you to connect to ports under 1024. Sorry.", true );
        return true;
    }
    char * ssl = strtok_r( NULL, " ", &savePtr );
    bool wantSSL = false;
    bool tlsV1 = false;
    if( ssl ) {
        wantSSL = strncmp(ssl,"ssl",3) == 0;
        if( strncmp(ssl,"sslv1",5) == 0 ) {wantSSL=tlsV1=true;}
        if( !wantSSL ) {
            NullStringToWebsockets( userState, ws, "Optional ssl argument bad, needs to be ssl or sslv1", true );
            return true;
        }
    }
    struct MudState * mud = CreateMud( userState, name, address, portNum, wantSSL, tlsV1, 32768, 800 );
    if( !mud ) {
        NullStringToWebsockets( userState, ws, "Failed to create a connection.", true );
        return true;
    }
    AddMud( userState, mud );
    if( ConnectMud( mud, appAllowNonRoutable ) ) {
        NullStringToWebsockets( userState, ws, "Failed to connect to remote.", true );
        return true;
    }
    PutMudFront( userState, mud );
    return false;
}

static bool loopbackCommand( struct CS_WebSocket *ws, struct UserState *state, const char *line, int length ) {
    state->loopback = !state->loopback;
    if( state->loopback ) {
        NullStringToWebsockets( state, NULL, "Loopback mode on. All text sent just comes right back.", true );
    } else {
        NullStringToWebsockets( state, NULL, "Loopback mode off.", true );
    }
    return false;
}

static bool recallCommand( struct CS_WebSocket *ws, struct UserState *state, const char *line, int length ) {
    char *temp = tempCopyWithNulls( line, length );
    char *savePtr;
    char *command = strtok_r( temp, " ", &savePtr );
    char *index = strtok_r( NULL, " ", &savePtr );
    if( !state || !state->front || !state->front->what ) return true;
    if( index == NULL ) {
        NullStringToWebsockets( state, ws, "Recall command needs a number of lines.", true );
        return true;
    }
    int numLines = strtol( index, NULL, 10 );
    if( numLines == 0 ) return false;
    char *filter = strtok_r( NULL, " ", &savePtr );
    NullStringToWebsockets( state, ws, "Recall Start", true );
    MudBackscrollToWebsockets( (struct MudState *)state->front->what, ws, numLines, filter, true, true );
    NullStringToWebsockets( state, ws, "Recall End", true );
    return false;
}

static bool disconnectCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int length ) {
    
    DisconnectFront( userState );

    return false;
}

bool killCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int lineLength ) {
    DeleteFront( userState );
    return false;
}

bool kickCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int lineLength ) {
    DisconnectOthers( userState, ws );
    return false;
}

bool helpCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int lineLength ) {
    NullStringToWebsockets( userState, NULL, "Navigate to the /help.html page", true );
    return false;
}

bool loremCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int lineLength ) {
    NullStringToWebsockets( userState, ws, loremIpsum, true );
    return false;
}

bool statusCommand( struct CS_WebSocket *ws, struct UserState *userState, const char *line, int lineLength ) {
    SendStatus( userState, true );
    return false;
}

static struct commandToHandler commands[] = {
    { "/info", 5, infoCommand },
    { "/connect", 8, connectCommand },
    { "/next", 5, nextCommand },
    { "/prev", 5, prevCommand },
    { "/pick", 5, pickCommand },
    { "/loopback", 9, loopbackCommand },
    { "/recall", 7, recallCommand },
    { "/dc", 3, disconnectCommand },
    { "/disconnect", 11, disconnectCommand },
    { "/kill", 5, killCommand },
    { "/kick", 5, kickCommand },
    { "/help", 5, helpCommand },
    { "/lorem", 6, loremCommand },
    { "/status", 7, statusCommand }
};

bool dealWithUserCommand( struct CS_WebSocket *ws, struct UserState *user, const char *line, int lineLength ) {
    int numCommands = CS_ARRAY_SIZE( commands );
    for( int i = 0; i < numCommands; ++i ) {
        if( memcmp( line, commands[i].command, commands[i].commandLength ) == 0 ) {
            return commands[i].executeMe( ws, user, line, lineLength );
        }
    }
    NullStringToWebsockets( user, NULL, CS_tempBuffSnprintf( 128, "Unrecognized Command: %.*s", lineLength, line ), true );
    return true;
}

bool dealWithUserInput( struct CS_WebSocket *ws, struct UserState *user, const struct CS_WebSocketFrame *frame ) {
    if( frame->payload != NULL && *(char*)frame->payload == '/' ) {
        dealWithUserCommand( ws, user, frame->payload, frame->payloadLength );
    } else {
        //Testing loopback state.
        if( user->loopback ) {
            TextToWebsockets( user, NULL, frame->payload, frame->payloadLength, true );
        } else {
            if( user->front ) {
                TextToFront( user, frame->payload, frame->payloadLength, true );
            } else {
                NullStringToWebsockets( user, NULL, "Not connected anywhere.", true );
            }
        }
    }
    return false;
}

#define SEND_FRAME(__WS__,__FRAME__,__GOTO__) if((__FRAME__)==NULL||CS_WS_pushFrame(__WS__,__FRAME__)) { goto __GOTO__; }

bool websocket( struct CS_ClientInfo *info ) {
    if ( CS_WS_requestWantsWebsocket(info) ) {
        const char *cookieValue = CS_serverGetRequestCookie( info, "session" );
        const void *currentSession = CS_hashtableGet( cheapSessions, cookieValue );
        struct CS_WebSocket *gws = CS_WS_create( info, NULL );
        struct CS_WebSocketFrame *returnFrame = NULL;
        char *copyBuff = NULL;
        struct UserState *user = (struct UserState *)currentSession;
        int printLength = 0;
        char *tempbuff;
        const struct CS_ListItem *theItem;
        if( gws ) {
            AddWebsocket( user, gws );
            struct CS_WebSocketFrame * nextFrame = NULL;
            while( true ) {
                nextFrame = CS_WS_nextIncomingFrame( gws );
                if( nextFrame == NULL ) break;
                switch( nextFrame->opcode ) {
                    //We must return a pong for any ping we get.
                    case CS_WS_OPCODE_PING:
                        returnFrame = CS_WS_createFrame( gws, CS_WS_OPCODE_PONG, false, nextFrame->payload, nextFrame->payloadLength );
                        SEND_FRAME( gws, returnFrame, ERROR_CLOSE );
                        returnFrame = NULL;
                        break;
                    case CS_WS_OPCODE_TEXT:
                        if( dealWithUserInput( gws, user, nextFrame ) )
                            goto ERROR_CLOSE;
                        break;
                    default:
                        break;
                }
                CS_WS_returnFrame( gws, nextFrame );
            }
ERROR_CLOSE:
            RemoveWebsocket( user, gws );
            if( nextFrame ) CS_WS_returnFrame( gws, nextFrame );
            CS_WS_destroy( gws );
        }
    }
    return true;
}

bool loginRedirectToHead( struct CS_ClientInfo *info, const char *sessionCookie ) {
    struct CS_HtmlNode *root = CS_htmlCreateRoot("html",2048);
    struct CS_HtmlNode *head = CS_htmlAddContainerAfter( root, "head" );
    struct CS_HtmlNode *meta = CS_htmlAddContainerAfter( head, "meta" );
    CS_htmlAddAttribute( meta, "charset", "utf-8" );
    struct CS_HtmlNode *title = CS_htmlAddContainerAfter( head, "title" );
    CS_htmlSetContents( title, CS_tempBuffSnprintf(1024, "webmud redirecting" ), false );
    struct CS_HtmlNode *body = CS_htmlAddContainerAfter( root, "body" );
    CS_htmlSetContents(body, "Redirecting.", false);
    struct CS_HtmlNode *script = CS_htmlAddContainerAfter( body, "script" );
    CS_htmlSetContents(script,"window.location = \"/\";",true);
    struct CS_StringBuilder *sb = CS_htmlToStringBuilder( root, 2048 );
    struct CS_Reply *reply = CS_serverCreateReply( info, CS_RESPONSE_200, CS_MIME_HTML, CS_SB_buffer( sb ), CS_SB_size( sb ) );
    CS_serverSetReplyCookie( reply, "session", sessionCookie, true );
    CS_serverDoReply( info, reply );
    CS_SB_free( sb );
    CS_htmlFree( root );
    return true;
}

bool loginAndReturnIndex( struct CS_ClientInfo *info, const char *sessionCookie ) {
    struct CS_Reply *reply = CS_serverCreateReply( info, CS_RESPONSE_200, CS_MIME_HTML, NULL, 0 );
    CS_serverSetReplyCookie( reply, "session", sessionCookie, true );
    return CS_serverPushFile( "root/index.html", info, 0, reply );
}

bool googleLogin( struct CS_ClientInfo *info ) {
    const char *g_csrf_header = CS_serverGetRequestCookie(info, "g_csrf_token");
    if( !g_csrf_header ) {
        CS_LOG_ERROR( "Login missing csrf token header." );
        return loginPageReturn(info);
    }
    const char *g_csrf_form = CS_serverGetRequestFormParameter(info, "g_csrf_token");
    if( !g_csrf_form ) {
        CS_LOG_ERROR( "Login missing csrf token form." );
        return loginPageReturn(info);
    }
    if( strcmp( g_csrf_header, g_csrf_form ) != 0 ) {
        CS_LOG_ERROR( "Login csrf different from form csrf." );
        return loginPageReturn(info);
    }
    const char *credential = CS_serverGetRequestFormParameter(info, "credential");
    if( !credential ) {
        CS_LOG_ERROR( "Login missing credential." );
        return loginPageReturn(info);
    }
    int nLen = strlen(credential);
    const struct CS_Jwt *jwt = CS_jwtParse( credential, nLen, 1024 );
    if( !jwt ) {
        CS_LOG_ERROR( "Failed to parse a jwt out of the credential." );
        CS_jwtFree(jwt);
        return loginPageReturn(info);
    }
    if( !CS_GS_jwtVerify(jwt) ) {
        CS_LOG_ERROR( "Failed to verify a jwt." );
        CS_jwtFree(jwt);
        return loginPageReturn(info);
    }

    struct CS_JsonNode *subject = CS_jsonNodeByPath( jwt->jsonPayload, "sub" );
    if( !subject ) {
        CS_jwtFree(jwt);
        return loginPageReturn(info);
    }
    char *googleId = strndup( (char*)CS_jsonNodeValueAsTempString( subject ), 64 );

    const void *sessionId = CS_hashtableGet( googleIdToSessionId, googleId );

    if( sessionId == CS_HASHTABLE_ERROR ) {
        sessionId = CS_uuid4StringTemp();
        struct UserState *user = CreateUserState( sessionId  );
        CS_hashtablePut( googleIdToSessionId, googleId, sessionId );
        CS_hashtablePut( cheapSessions, sessionId, user );
    }

    CS_jwtFree( jwt );
    
    return loginRedirectToHead( info, (char*)sessionId );
}

bool autoLoginUtil( struct CS_ClientInfo *info ) {
    const void * sessionId = CS_uuid4StringTemp();
    struct UserState *user = CreateUserState( sessionId );
    CS_hashtablePut( cheapSessions, sessionId, user );

    return loginRedirectToHead( info, (char*)sessionId );
}

bool logout( struct CS_ClientInfo *info ) {
    struct CS_Reply *reply = CS_serverCreateReply( info, CS_RESPONSE_200, CS_MIME_HTML, NULL, 0 );
    char *tempUuid4 = (char*)CS_uuid4StringTemp();
    if( tempUuid4 ) *tempUuid4 = 'L';
    CS_serverSetReplyCookie( reply, "session", tempUuid4, true );
    return CS_serverPushFile( "root/loggedoff.html", info, 0, reply );
}

bool loginPageReturn( struct CS_ClientInfo *info ) {
    struct CS_HtmlNode *root = CS_htmlCreateRoot("html",2048);
    struct CS_HtmlNode *head = CS_htmlAddContainerAfter( root, "head" );
    struct CS_HtmlNode *meta = CS_htmlAddContainerAfter( head, "meta" );
    CS_htmlAddAttribute( meta, "charset", "utf-8" );
    struct CS_HtmlNode *title = CS_htmlAddContainerAfter( head, "title" );
    CS_htmlSetContents( title, "webmud login page", false);
    struct CS_HtmlNode *body = CS_htmlAddContainerAfter( root, "body" );
    struct CS_HtmlNode *script = CS_htmlAddContainerAfter(body, "script");
    CS_htmlAddAttribute( script, "async", NULL );
    CS_htmlAddAttribute( script, "src", "https://accounts.google.com/gsi/client" );
    struct CS_HtmlNode *div = CS_htmlAddContainerAfter( body, "div" );
    CS_htmlAddAttribute( div, "id", "g_id_onload" );
    CS_htmlAddAttribute( div, "data-client_id", CS_GS_getClientID() );

    const char *host = CS_serverGetRequestHeader(info,"Host");
    if( host == NULL ) host = "localhost";
    CS_htmlAddAttribute( div, "data-login_uri",
           CS_tempBuffSnprintf( 1024, "http%s://%s/googlelogin", 
               info->ssl?"s":"", host ) );
    CS_htmlAddAttribute( div, "data-auto_prompt", "false" );
    div = CS_htmlAddContainerAfter( body, "div" );
    CS_htmlAddAttribute( div, "class", "g_id_signin" );
    CS_htmlAddAttribute( div, "data-type", "standard" );
    CS_htmlAddAttribute( div, "data-size", "large" );
    CS_htmlAddAttribute( div, "data-theme", "outline" );
    CS_htmlAddAttribute( div, "data-text", "sign_in_with" );
    CS_htmlAddAttribute( div, "data-shape", "rectangular" );
    CS_htmlAddAttribute( div, "data-logo_alignment", "left" );
    struct CS_StringBuilder *sb = CS_htmlToStringBuilder( root, 2048 );
    struct CS_Reply *reply = CS_serverCreateReply( info, CS_RESPONSE_200, CS_MIME_HTML, CS_SB_buffer( sb ), CS_SB_size( sb ) );
    CS_serverDoReply( info, reply );
    CS_serverReturnReply( info, reply );
    CS_SB_free( sb );
    CS_htmlFree( root );
    return true;
}


