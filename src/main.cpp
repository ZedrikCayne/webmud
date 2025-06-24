#include <stdio.h>

#include <stdlib.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <signal.h>

#include <crankshaft/logger.h>
#include <crankshaft/commandline.h>
#include <crankshaft/server.h>
#include <crankshaft/tempbuff.h>
#include <crankshaft/test.h>
#include <crankshaft/html.h>
#include <crankshaft/googleservices.h>
#include <crankshaft/mime.h>
#include <crankshaft/jwt.h>
#include <crankshaft/json.h>
#include <crankshaft/storage.h>
#include <crankshaft/jwtkeychain.h>
#include <crankshaft/ssl.h>

#include <crankshaft/websocket.h>

int acceptSocket = 0;

static bool onlyFails = false;
static char defaultServerName[] = "webmud";
static bool suppressErrors = false;
static bool wantHelp = false;
static bool noWarn = false;
static bool quiet = false;
static bool info = false;
static bool verbose = false;
static bool trace = false;
static bool runAsDaemon = false;
static int portNum = 8080;
static char *serverName = defaultServerName;
static char *logFile = NULL;
static char defaultFileServingDir[] = "./root";
static char *fileServingDir = defaultFileServingDir;
static char defaultFileServingFile[] = "index.html";
static char *fileServingFile = defaultFileServingFile;
static char *certFile = NULL;
static char *keyFile = NULL;
static char *selfSignHostname = NULL;
static int cacheTimeInSeconds = 0;

CS_ARG_DEF(wantHelp,CS_ARG_CMP("-?","-help","--help"),"Prints this help");
CS_ARG_DEF(onlyFails,CS_ARG_CMP("--only-fails"), "Only print fails during unit testing.");
CS_ARG_DEF(noWarn,CS_ARG_CMP("-w","--no-warn"),"No warning logs.");
CS_ARG_DEF(quiet,CS_ARG_CMP("-q","--quiet"),"Quiet logs (Warnings, errors and 'loud logs' only.)");
CS_ARG_DEF(info,CS_ARG_CMP("-i","--info"),"Info logs.");
CS_ARG_DEF(verbose,CS_ARG_CMP("-v", "--verbose"),"Verbose logs");
CS_ARG_DEF(trace,CS_ARG_CMP("-t", "--trace"),"Trace logs");
CS_ARG_DEF(suppressErrors,CS_ARG_CMP("--suppress-errors"),"Supress error logs (useful during --test)");
CS_ARG_DEF(portNum,CS_ARG_CMP("-p", "--port"),"Port Number");
CS_ARG_DEF(serverName,CS_ARG_CMP("-n","--name"),"Name of server");
CS_ARG_DEF(cacheTimeInSeconds, CS_ARG_CMP("--cache"),"Cache time of default file server (0 is no cache)");
CS_ARG_DEF(runAsDaemon,CS_ARG_CMP("-d","--daemon"),"Run as daemon");
CS_ARG_DEF(logFile,CS_ARG_CMP("-l","--log"),"Log to file");
CS_ARG_DEF(fileServingFile,CS_ARG_CMP("-f","--default-file"), "Default file when using GET on a directory.");
CS_ARG_DEF(fileServingDir,CS_ARG_CMP("--dir","--default-directory"), "Default directory for default file server.");
CS_ARG_DEF(keyFile,CS_ARG_CMP("-k","--key"), "pemfile for ssl private key.");
CS_ARG_DEF(certFile,CS_ARG_CMP("-c","--certificate"), "pemfile for certificate");
CS_ARG_DEF(selfSignHostname,CS_ARG_CMP("--self-sign"), "create a self signed certificate for provided host");

const struct CS_ArgElement myArgs[] = 
    { CS_ARG_ELEMENT(wantHelp,CS_BOOL_ARG),
      CS_ARG_ELEMENT(onlyFails,CS_BOOL_ARG),
      CS_ARG_ELEMENT(noWarn,CS_BOOL_ARG),
      CS_ARG_ELEMENT(quiet,CS_BOOL_ARG),
      CS_ARG_ELEMENT(info,CS_BOOL_ARG),
      CS_ARG_ELEMENT(verbose,CS_BOOL_ARG),
      CS_ARG_ELEMENT(trace,CS_BOOL_ARG),
      CS_ARG_ELEMENT(suppressErrors,CS_BOOL_ARG),
      CS_ARG_ELEMENT(portNum,CS_INT_ARG),
      CS_ARG_ELEMENT(serverName,CS_STRING_ARG),
      CS_ARG_ELEMENT(runAsDaemon,CS_BOOL_ARG),
      CS_ARG_ELEMENT(logFile,CS_STRING_ARG),
      CS_ARG_ELEMENT(fileServingFile,CS_STRING_ARG),
      CS_ARG_ELEMENT(fileServingDir,CS_STRING_ARG),
      CS_ARG_ELEMENT(cacheTimeInSeconds,CS_INT_ARG),
      CS_ARG_ELEMENT(keyFile,CS_STRING_ARG),
      CS_ARG_ELEMENT(certFile,CS_STRING_ARG),
      CS_ARG_ELEMENT(selfSignHostname,CS_STRING_ARG)
    };

struct CS_ArgTable myCS_ArgTable = { sizeof(myArgs)/sizeof(CS_ArgElement), 0, NULL, myArgs };

void PrintHelp() {
    CS_argsPrint(&myCS_ArgTable);
}

void PrintHeader() {
    //      12345678901234567890123456789012345678901234567890123456789012345678901234567890
    printf("crankshaft, around which the world turns.        \\\n");
    printf("                              ==     ==    ==     \\\n");
    printf("                             /  \\   /  \\  /  \\     \\\n");
    printf(" =========================   |  |   |  |  |   ======\\===\n");
    printf("                          \\  /  \\  /   \\  /          \\\n");
    printf("                           ==    ==     ==            \\\n");
    printf("                                                       \\\n");
    printf("\n");
}

static bool GotInterrupt = false;
static bool GotHup = false;

static void terminateHandler(int sig) {
    signal(sig, SIG_IGN);
    GotInterrupt = true;
    signal(SIGTERM, terminateHandler);
}

static void interruptHandler(int sig) {
    signal(sig, SIG_IGN);
    GotInterrupt = true;
    signal(SIGINT, interruptHandler);
}

static void hupHandler(int sig) {
    signal(sig, SIG_IGN);
    GotHup = true;
    signal(SIGHUP, hupHandler);
}

void hupOnMainThread() {
    CS_LOG_TRACE("HUP");
    GotHup = false;
}

void dcCallback( struct CS_ClientInfo *info ) {
    CS_LOG_TRACE("Disconnecting.");
}

bool jwtInfoReturn( struct CS_ClientInfo *info, const struct CS_Jwt *jwt, const char *csrf );
bool loginPageReturn( struct CS_ClientInfo *info );
bool cookieFilter( struct CS_ClientInfo *info ) {
    const char *cookieValue = CS_serverGetRequestCookie( info, "session" );
    if( cookieValue != NULL ) {
        CS_LOG_TRACE("session cookie set %s", cookieValue );
        return false;
    }
    return loginPageReturn(info);
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
        return loginPageReturn(info);
    }
    if( !CS_GS_jwtVerify(jwt) ) {
        CS_LOG_ERROR( "Failed to verify a jwt." );
        return loginPageReturn(info);
    }

    bool returnValue = jwtInfoReturn(info, jwt, g_csrf_header );

    CS_jwtFree( jwt );
    
    return returnValue;
}

bool fudge( struct CS_ClientInfo *info ) {
    if( info->disconnectCallback == NULL ) {
        info->disconnectCallback = dcCallback;
    }
    return CS_serverDiagnostic200(info);
}

bool websocket( struct CS_ClientInfo *info ) {
    if ( CS_WS_requestWantsWebsocket(info) ) {
        struct CS_WebSocket *gws = CS_WS_create( info, NULL );
        struct CS_WebSocketFrame *returnFrame = NULL;
        char *copyBuff = NULL;
        if( gws ) {
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
                        returnFrame = NULL;
                        
                        break;
                    case CS_WS_OPCODE_TEXT:
                        CS_LOG_TRACE("Incoming text frame. %s", CS_WS_describeFrame(nextFrame));
                        if( nextFrame->payloadLength > 0 ) {
                            copyBuff = (char*)CS_tempMemCopy( nextFrame->payload, nextFrame->payloadLength );
                        } else {
                            copyBuff = NULL;
                        }
                        if( copyBuff ) {
                            for( int i = 0; i < nextFrame->payloadLength; ++i ) {
                                if( copyBuff[i] >= 'a' && copyBuff[i] <= 'z' ) {
                                    copyBuff[i] -= 32;
                                }
                            }
                        }
                        returnFrame = CS_WS_createFrame( gws, CS_WS_OPCODE_TEXT, false, copyBuff, nextFrame->payloadLength );
                        copyBuff = NULL;
                        if( returnFrame == NULL || CS_WS_pushFrame( gws, returnFrame ) ) {
                            goto ERROR_CLOSE;
                        }
                        returnFrame = NULL;
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



bool doQuit( struct CS_ClientInfo *info ) {
    GotInterrupt = true;
    return CS_serverDiagnostic200(info);
}

bool jwtInfoReturn( struct CS_ClientInfo *info, const struct CS_Jwt *jwt, const char *csrf ) {
    struct CS_HtmlNode *root = CS_htmlCreateRoot("html",2048);
    struct CS_HtmlNode *head = CS_htmlAddContainerAfter( root, "head" );
    struct CS_HtmlNode *meta = CS_htmlAddContainerAfter( head, "meta" );
    CS_htmlAddAttribute( meta, "charset", "utf-8" );
    struct CS_HtmlNode *title = CS_htmlAddContainerAfter( head, "title" );
    CS_htmlSetContents( title, CS_tempBuffSnprintf(1024, "JWT info page", serverName ) );
    struct CS_HtmlNode *body = CS_htmlAddContainerAfter( root, "body" );
    struct CS_HtmlNode *div1 = CS_htmlAddContainerAfter( body, "div" );
    struct CS_HtmlNode *header = CS_htmlAddContainerAfter( div1, "h4" );
    CS_htmlSetContents( header, "JWT Header" );
    struct CS_HtmlNode *code = CS_htmlAddContainerAfter( div1, "code" );
    if( jwt->jsonHeader != NULL ) {
        CS_htmlSetContents( code, CS_jsonNodePrintableTemp( jwt->jsonHeader ) );
    } else {
        CS_htmlSetContents( code, "Json Header is null!" );
    }
    struct CS_HtmlNode *div2 = CS_htmlAddNext( div1, "div" );
    struct CS_HtmlNode *header2 = CS_htmlAddContainerAfter( div2, "h4" );
    CS_htmlSetContents( header2, "JWT Payload" );
    struct CS_HtmlNode *code2 = CS_htmlAddContainerAfter( div2, "code" );   
    if( jwt->jsonPayload != NULL ) {
        CS_htmlSetContents( code2, CS_jsonNodePrintableTemp( jwt->jsonPayload ) );
    } else {
        CS_htmlSetContents( code2, "Json Header is null!" );
    }
    struct CS_HtmlNode *div3 = CS_htmlAddNext( div2, "div" );
    struct CS_HtmlNode *header3 = CS_htmlAddContainerAfter( div3, "h4" );
    CS_htmlSetContents( header3, "CSRF value" );
    struct CS_HtmlNode *code3 = CS_htmlAddContainerAfter( div3, "code" );
    if( csrf != NULL ) {
        CS_htmlSetContents( code3, csrf );
    } else {
        CS_htmlSetContents( code3, "NULL" );
    }
    struct CS_StringBuilder *sb = CS_htmlToStringBuilder( root, 2048 );
    struct CS_Reply *reply = CS_serverCreateReply( info, CS_RESPONSE_200, CS_MIME_HTML, CS_SB_buffer( sb ), CS_SB_size( sb ) );
    CS_serverDoReply( info, reply );
    CS_serverReturnReply( info, reply );
    CS_SB_free( sb );
    CS_htmlFree( root );
    return true;
}

bool loginPageReturn( struct CS_ClientInfo *info ) {
    struct CS_HtmlNode *root = CS_htmlCreateRoot("html",2048);
    struct CS_HtmlNode *head = CS_htmlAddContainerAfter( root, "head" );
    struct CS_HtmlNode *meta = CS_htmlAddContainerAfter( head, "meta" );
    CS_htmlAddAttribute( meta, "charset", "utf-8" );
    struct CS_HtmlNode *title = CS_htmlAddContainerAfter( head, "title" );
    CS_htmlSetContents( title, CS_tempBuffSnprintf(1024, "%s login page", serverName ) );
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

struct CS_Route serverRoutes[] = {
    { CS_HTTP_METHOD_GET,  CS_ROUTE_TYPE_EXACT, 0, "/googlelogin", googleLogin},
    { CS_HTTP_METHOD_POST, CS_ROUTE_TYPE_EXACT, 0, "/googlelogin", googleLogin},
    { CS_HTTP_METHOD_ANY,  CS_ROUTE_TYPE_FILTER, 0, "", cookieFilter},
    { CS_HTTP_METHOD_ANY,  CS_ROUTE_TYPE_PREFIX, 0, "/ws", websocket},
    { CS_HTTP_METHOD_HEAD, CS_ROUTE_TYPE_WILDCARD, 0, "", CS_serverFileServer },
    { CS_HTTP_METHOD_GET,  CS_ROUTE_TYPE_WILDCARD, 0, "", CS_serverFileServer },
};

int main(int argc, char *argv[] ) {
    const char * error = CS_argsParse(argc, argv, &myCS_ArgTable);
    if( error != NULL || wantHelp ) {
        PrintHeader();
        if( error != NULL ) CS_LOG_ERROR("%s", error);
        PrintHelp();
        return -1;
    }
    int countLogMods = 0;
    if( noWarn ) ++countLogMods;
    if( quiet ) ++countLogMods;
    if( verbose ) ++countLogMods;
    if( trace ) ++countLogMods;

    if( countLogMods > 1 ) CS_LOG_WARN("More than one of trace, quiet, noWarn or verbose specified. The loudest one will rule");

    if( noWarn )  { CS_LOG_VERBOSE_BOOL=false; CS_LOG_INFO_BOOL=false; CS_LOG_QUIET_BOOL=true ; CS_LOG_TRACE_BOOL=false; CS_LOG_WARN_BOOL=false; }
    if( quiet )   { CS_LOG_VERBOSE_BOOL=false; CS_LOG_INFO_BOOL=false; CS_LOG_QUIET_BOOL=true ; CS_LOG_TRACE_BOOL=false; CS_LOG_WARN_BOOL=true;}
    if( verbose ) { CS_LOG_VERBOSE_BOOL=true ; CS_LOG_INFO_BOOL=false; CS_LOG_QUIET_BOOL=false; CS_LOG_TRACE_BOOL=false; CS_LOG_WARN_BOOL=true; }
    if( info )    { CS_LOG_VERBOSE_BOOL=true ; CS_LOG_INFO_BOOL=true ; CS_LOG_QUIET_BOOL=false; CS_LOG_TRACE_BOOL=false; CS_LOG_WARN_BOOL=true; }
    if( trace )   { CS_LOG_VERBOSE_BOOL=true ; CS_LOG_INFO_BOOL=true ; CS_LOG_QUIET_BOOL=false; CS_LOG_TRACE_BOOL=true ; CS_LOG_WARN_BOOL=true; }
    if( suppressErrors ) { CS_LOG_ERROR_BOOL = false; }

    if( logFile != NULL ) {
        if( CS_logInit( logFile ) ) {
            CS_LOG_ERROR("Logging subsystem failed to init. Bailing!");
            return -1;
        }
    }

    CS_sslInit();

    CS_tempAllocateGlobal(2*1024*1024);

    if( CS_GS_initWithEnvironmentVariable( "GOOGLE_JSON" ) ) {
        CS_LOG_WARN("GOOGLE_JSON not defined in the environment. Anything depending on google services json being initialized will fail.");
    }

    CS_LOG_INFO("Server Name: %s", serverName);
    CS_LOG_INFO("Port Number is %d", portNum);

    signal(SIGINT, interruptHandler);
    signal(SIGHUP, hupHandler);
    signal(SIGTERM, terminateHandler);

    CS_LOG_INFO("Starting web server.");
    const struct CS_Storage *keysCacheBackingStorage = CS_storageOpen( "KEY_WEB_CACHE", "file=/tmp/crankshaft_key.sqlite", CS_STORAGE_BACKEND_SQLITE );
    CS_jwtkeychainInit( keysCacheBackingStorage );

    struct CS_WebServer *server = CS_serverStart( portNum, certFile, keyFile, selfSignHostname, fileServingDir, fileServingFile, cacheTimeInSeconds, serverRoutes, sizeof(serverRoutes)/sizeof(serverRoutes[0]) );
    if( server != NULL ) {
        CS_LOG_INFO("Server started at port %d", server->serverPort);
        while(!GotInterrupt) {
            if( GotHup ) hupOnMainThread();
            sleep(1);
        }
        CS_serverKill(server);
    } else {
        CS_LOG_ERROR("Server failed to start...");
    }

    if( logFile != NULL ) CS_logKill();
    CS_GS_kill();
    CS_sslKill();
    CS_tempFreeGlobal();
    return 0;
}
