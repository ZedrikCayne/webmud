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
#include "webmud.h"

static struct CS_HashTable *cheapSessions = NULL;
static struct CS_HashTable *googleIdToSessionId = NULL;

bool startApplication() {
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
        CS_LOG_TRACE("WEBMUD session cookie is %s", cookieValue );
        const void *currentSession = CS_hashtableGet( cheapSessions, cookieValue );
        if( currentSession != CS_HASHTABLE_ERROR && currentSession != NULL ) {
            CS_LOG_TRACE("WEBMUD cookie in session table %s", cookieValue );
            return false;
        }
        CS_LOG_TRACE("WEBMUD cookie not in session table %s", cookieValue );
    }
    return loginPageReturn(info);
}

static struct CS_WebSocketFrame *backscrollToFrame( struct CS_WebSocket *ws,
        const struct CS_ListItem *item ) {
    struct BackscrollLine *line = (struct BackscrollLine *)item->what;
    struct CS_WebSocketFrame *returnFrame = CS_WS_createFrame( ws, CS_WS_OPCODE_TEXT, false, line->head, line->size);
    return returnFrame;
}

bool websocket( struct CS_ClientInfo *info ) {
    if ( CS_WS_requestWantsWebsocket(info) ) {
        const char *cookieValue = CS_serverGetRequestCookie( info, "session" );
        const void *currentSession = CS_hashtableGet( cheapSessions, cookieValue );
        struct CS_WebSocket *gws = CS_WS_create( info, NULL );
        struct CS_WebSocketFrame *returnFrame = NULL;
        char *copyBuff = NULL;
        struct Backscroll *bs = (struct Backscroll *)currentSession;
        CS_LOG_INFO("Backscroll: %p", bs);
        int printLength = 0;
        char *tempbuff;
        const struct CS_ListItem *theItem;
        if( gws ) {
            CS_LIST_ITER( bs->backscrollLines, anItem ) {
                returnFrame = backscrollToFrame( gws, anItem );
                if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                    goto ERROR_CLOSE;
                }
            }
            while( true ) {
                struct CS_WebSocketFrame * nextFrame = CS_WS_nextIncomingFrame( gws );
                if( nextFrame == NULL ) return true;
                switch( nextFrame->opcode ) {
                    //We must return a pong for any ping we get.
                    case CS_WS_OPCODE_PING:
                        CS_LOG_TRACE("We got incoming ping. %s", CS_WS_describeFrame(nextFrame));
                        returnFrame = CS_WS_createFrame( gws, CS_WS_OPCODE_PONG, false, nextFrame->payload, nextFrame->payloadLength );
                        if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                            goto ERROR_CLOSE;
                        }
                        CS_LOG_INFO("Backscroll: %p", bs);
                        returnFrame = NULL;
                        
                        break;
                    case CS_WS_OPCODE_TEXT:
                        CS_LOG_TRACE("Incoming text frame. %s", CS_WS_describeFrame(nextFrame));
                        FeedBackscroll( bs, nextFrame->payload, nextFrame->payloadLength );
                        tempbuff = CS_tempBuffSnprintf( 128, "BS: %d lines. first line:\r\n", CS_listCount( bs->backscrollLines ) );
                        if( tempbuff ) {
                            returnFrame = CS_WS_createFrame( gws, CS_WS_OPCODE_TEXT, false, tempbuff, strlen( tempbuff ) );
                            if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                                goto ERROR_CLOSE;
                            }
                        }
                        theItem = CS_listGetHead(bs->backscrollLines);
                        if( theItem ) {
                            returnFrame = backscrollToFrame( gws, theItem );
                            if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                                goto ERROR_CLOSE;
                            }
                        }
                        tempbuff = CS_tempBuffSnprintf( 128, "BS: last ~2 lines:\r\n", CS_listCount( bs->backscrollLines ) );
                        if( tempbuff ) {
                            returnFrame = CS_WS_createFrame( gws, CS_WS_OPCODE_TEXT, false, tempbuff, strlen( tempbuff ) );
                            if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                                goto ERROR_CLOSE;
                            }
                        }
                        theItem = CS_listGetTail( bs->backscrollLines );
                        if( theItem && theItem->last ) theItem = theItem->last;
                        if( theItem ) {
                            returnFrame = backscrollToFrame( gws, theItem );
                            if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                                goto ERROR_CLOSE;
                            }
                        }
                        if( theItem && theItem->next ) {
                            if( theItem->what ) CS_LOG_TRACE("LINE: %d %.*s", theItem->size, theItem->size, theItem->what);
                            theItem = theItem->next;
                            returnFrame = backscrollToFrame( gws, theItem );
                            if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                                goto ERROR_CLOSE;
                            }
                        }

                        
                        break;
                    default:
                        CS_LOG_TRACE( "%s", CS_WS_describeFrame( nextFrame ) );
                        break;
                }

                CS_WS_returnFrame( gws, nextFrame );
            }
ERROR_CLOSE:
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
        CS_LOG_ERROR( "WEBMUD Missing subject." );
        CS_jwtFree(jwt);
        return loginPageReturn(info);
    }
    char *googleId = strndup( (char*)CS_jsonNodeValueAsTempString( subject ), 64 );


    const void *sessionId = CS_hashtableGet( googleIdToSessionId, googleId );

    if( sessionId == CS_HASHTABLE_ERROR ) {
        struct Backscroll *backscroll = CreateBackscroll( 32768, 500 );
        CS_LOG_TRACE( "Backscroll %p", backscroll );
        sessionId = CS_uuid4String();
        CS_hashtablePut( googleIdToSessionId, googleId, sessionId );
        CS_hashtablePut( cheapSessions, sessionId, backscroll );
    }

    CS_jwtFree( jwt );
    
    return loginRedirectToHead( info, (char*)sessionId );
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


