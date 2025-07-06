#ifndef __webmuddoth__
#define __webmuddoth__
#include <stdbool.h>
#include <crankshaft/server.h>
#include <crankshaft/websocket.h>
#include <crankshaft/socket.h>
#include "userstate.h"

#ifdef __cplusplus
extern "C" {
#endif

bool startApplication(bool autoLogin, bool allowNonRoutable);
void killApplication();

//Filter for session cookie. Returns a login page if cookie does not exist.
bool cookieFilter( struct CS_ClientInfo *info );
//The login page
bool loginPageReturn( struct CS_ClientInfo *info );
//Page that redirects to / while setting the session cookie
bool loginRedirectToHead( struct CS_ClientInfo *info, const char *sessionCookie );
//Does the work of checking a login, returns a login page on fail.
bool googleLogin( struct CS_ClientInfo *info );
//Main work once we have a session.
bool websocket( struct CS_ClientInfo *info );
//Auto login for debug
bool autoLoginUtil( struct CS_ClientInfo *info );
//Log off
bool logout( struct CS_ClientInfo *info );
//Anonymous logins
bool anonymousLogin( struct CS_ClientInfo *info );
//Add or reset user
bool AddOrResetUser( const char *name );

#ifdef __cplusplus
}
#endif
#endif
