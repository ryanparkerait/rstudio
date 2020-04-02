/*
 * ServerAuthCommon.cpp
 *
 * Copyright (C) 2009-20 by RStudio, PBC
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#ifndef SERVER_AUTH_COMMON_CPP
#define SERVER_AUTH_COMMON_CPP

#include <server/auth/ServerAuthCommon.hpp>
#include <server/auth/ServerAuthHandler.hpp>

#include <core/http/URL.hpp>
#include <core/http/Cookie.hpp>
#include <core/http/CSRFToken.hpp>

#include <server_core/http/SecureCookie.hpp>

#include <server/ServerConstants.hpp>
#include <server/ServerOptions.hpp>

#include <monitor/MonitorClient.hpp>

#include <server/auth/ServerValidateUser.hpp>

#include "../ServerLoginPages.hpp"

using namespace rstudio::core;

namespace rstudio {
namespace server {
namespace auth {
namespace common {

std::string getUserIdentifier(const core::http::Request& request)
{
   return core::http::secure_cookie::readSecureCookie(request, kUserIdCookie);
}

bool mainPageFilter(const core::http::Request& request,
                    core::http::Response* pResponse,
                    UserIdentifierGetter userIdentifierGetter)
{
   // check for user identity, if we have one then allow the request to proceed
   std::string userIdentifier = userIdentifierGetter(request);
   if (userIdentifier.empty())
   {
      // otherwise redirect to sign-in
      redirectToLoginPage(request, pResponse, request.uri());
      return false;
   }
   return true;
}

void signInThenContinue(const core::http::Request& request,
                        core::http::Response* pResponse)
{
   redirectToLoginPage(request, pResponse, request.uri());
}

void refreshCredentialsThenContinue(
            boost::shared_ptr<core::http::AsyncConnection> pConnection)
{
   // no silent refresh possible so delegate to sign-in and continue
   signInThenContinue(pConnection->request(), &pConnection->response());
   pConnection->writeResponse();
}

void signIn(const core::http::Request& request,
            core::http::Response* pResponse,
            const std::string& templatePath,
            const std::string& formAction,
            std::map<std::string,std::string> variables /*= {}*/)
{
   clearSignInCookies(request, pResponse);
   loadLoginPage(request, pResponse, templatePath, formAction, variables);
}

ErrorType checkUser(const std::string& username, bool authenticated)
{
   // ensure user is valid
   if (!server::auth::validateUser(username))
   {
      // notify monitor of failed login
      using namespace monitor;
      client().logEvent(Event(kAuthScope,
                              kAuthLoginFailedEvent,
                              "",
                              username));

      return kErrorUserUnauthorized;
   }

   if (!authenticated) {
      // register failed login with monitor
      using namespace monitor;
      client().logEvent(Event(kAuthScope,
                              kAuthLoginFailedEvent,
                              "",
                              username));

      return kErrorInvalidLogin;
   }

   using namespace monitor;
   client().logEvent(Event(kAuthScope,
                           kAuthLoginEvent,
                           "",
                           username));

   return kErrorNone;
}

bool doSignIn(const core::http::Request& request,
              core::http::Response* pResponse,
              const std::string& username,
              std::string appUri,
              bool persist,
              bool authenticated /*= true*/)
{
   ErrorType error = checkUser(username, authenticated);
   if (error != kErrorNone)
   {
      redirectToLoginPage(request, pResponse, appUri, error);
      return false;
   }
   // Ensure the appUri is always rooted to not
   // get appended to the existing browser URL.
   // Security: This also prevents manipulation
   // of the appUri outside of the server's domain.
   if (appUri.empty() || appUri[0] != '/')
   {
      appUri = "/" + appUri;
   }
   setSignInCookies(request, username, persist, pResponse);
   pResponse->setMovedTemporarily(request, appUri);
   return true;
}

std::string signOut(const core::http::Request& request,
                    core::http::Response* pResponse,
                    UserIdentifierGetter userIdentifierGetter,
                    std::string signOutUrl)
{
   // validate sign-out request
   if (!core::http::validateCSRFForm(request, pResponse))
   {
      return "";
   }
   // register logout with monitor if we have the username
   std::string userIdentifier = userIdentifierGetter(request);
   std::string username;
   if (!userIdentifier.empty())
   {
      username = auth::handler::userIdentifierToLocalUsername(userIdentifier);

      using namespace monitor;
      client().logEvent(Event(kAuthScope,
                              kAuthLogoutEvent,
                              "",
                              username));
   }

   // invalidate the auth cookie so that it can no longer be used
   clearSignInCookies(request, pResponse);
   auth::handler::invalidateAuthCookie(request.cookieValue(kUserIdCookie));

   // adjust sign out url point internally
   if (!signOutUrl.empty() && signOutUrl[0] == '/')
   {
      signOutUrl = core::http::URL::uncomplete(request.uri(), signOutUrl);
   }
   pResponse->setMovedTemporarily(request, signOutUrl);
   return username;
}

bool isSecureCookie(const core::http::Request& request)
{
   bool secureCookie = options().authCookiesForceSecure() ||
                     options().getOverlayOption("ssl-enabled") == "1" ||
                     boost::algorithm::starts_with(request.absoluteUri(), "https");
   return secureCookie;                      
}

void clearSignInCookies(const core::http::Request& request,
                        core::http::Response* pResponse)
{
   core::http::secure_cookie::remove(request,
                                     kUserIdCookie,
                                     "/",
                                     pResponse,
                                     isSecureCookie(request));

   core::http::secure_cookie::remove(request,
                                     kUserListCookie,
                                     "/",
                                     pResponse,
                                     isSecureCookie(request));

   if (options().authTimeoutMinutes() > 0)
   {
      core::http::secure_cookie::remove(request,
                                        kPersistAuthCookie,
                                        "/",
                                        pResponse,
                                        isSecureCookie(request));
   }
}

void setSignInCookies(const core::http::Request& request,
                      const std::string& userIdentifier,
                      bool staySignedIn,
                      core::http::Response* pResponse,
                      bool reuseCsrf /*= false*/)
{
   std::string csrfToken = reuseCsrf ? request.cookieValue(kCSRFTokenCookie) : std::string();

   int staySignedInDays = server::options().authStaySignedInDays();
   int authTimeoutMinutes = server::options().authTimeoutMinutes();

   bool secureCookie = isSecureCookie(request);

   if (authTimeoutMinutes == 0)
   {
      // legacy auth expiration - users do not idle
      // and stay signed in for multiple days
      // not very secure, but maintained for those users that want this
      // optional persistance beyond the browser session
      boost::optional<boost::gregorian::days> expiry;
      if (staySignedIn)
         expiry = boost::gregorian::days(staySignedInDays);
      else
         expiry = boost::none;

      // set the cookie
      core::http::secure_cookie::set(kUserIdCookie,
                                     userIdentifier,
                                     request,
                                     boost::posix_time::hours(24 * staySignedInDays),
                                     expiry,
                                     "/",
                                     pResponse,
                                     secureCookie);

      // set a cookie that is tied to the specific user list we have written
      // if the user list ever has conflicting changes (e.g. a user is locked),
      // the user will be forced to sign back in
      core::http::secure_cookie::set(kUserListCookie,
                                     auth::handler::overlay::getUserListCookieValue(),
                                     request,
                                     boost::posix_time::hours(24*staySignedInDays),
                                     expiry,
                                     "/",
                                     pResponse,
                                     secureCookie);

      // add cross site request forgery detection cookie
      core::http::setCSRFTokenCookie(request, expiry, csrfToken, secureCookie, pResponse);
   }
   else
   {
      // new auth expiration - users are forced to sign in
      // after being idle for authTimeoutMinutes amount
      boost::optional<boost::posix_time::time_duration> expiry;
      if (staySignedIn)
         expiry = boost::posix_time::minutes(authTimeoutMinutes);
      else
         expiry = boost::none;

      // set the secure user id cookie
      core::http::secure_cookie::set(kUserIdCookie,
                                     userIdentifier,
                                     request,
                                     boost::posix_time::minutes(authTimeoutMinutes),
                                     expiry,
                                     "/",
                                     pResponse,
                                     secureCookie);

      // set a cookie that is tied to the specific user list we have written
      // if the user list ever has conflicting changes (e.g. a user is locked),
      // the user will be forced to sign back in
      core::http::secure_cookie::set(kUserListCookie,
                                     auth::handler::overlay::getUserListCookieValue(),
                                     request,
                                     boost::posix_time::minutes(authTimeoutMinutes),
                                     expiry,
                                     "/",
                                     pResponse,
                                     secureCookie);

      // set a cookie indicating whether or not we should persist the auth cookie
      // when it is automatically refreshed
      core::http::Cookie persistCookie(request,
                                       kPersistAuthCookie,
                                       staySignedIn ? "1" : "0",
                                       "/",
                                       true,
                                       secureCookie);
      persistCookie.setExpires(boost::posix_time::minutes(authTimeoutMinutes));
      pResponse->addCookie(persistCookie);

      core::http::setCSRFTokenCookie(request, expiry, csrfToken, secureCookie, pResponse);
   }
}

void prepareHandler(handler::Handler& handler,
                    core::http::UriHandlerFunction signInArg,
                    const std::string& signOutUrl,
                    UserIdentifierToLocalUsernameGetter userIdentifierToLocalUsernameArg,
                    UserIdentifierGetter getUserIdentifierArg /*= NULL*/)
{
   if (!getUserIdentifierArg)
   {
      getUserIdentifierArg = boost::bind(getUserIdentifier, _1);
   }
   handler.getUserIdentifier = boost::bind(getUserIdentifierArg, _1);
   handler.userIdentifierToLocalUsername = userIdentifierToLocalUsernameArg;
   handler.mainPageFilter = boost::bind(mainPageFilter, _1, _2, getUserIdentifierArg);
   handler.signInThenContinue = signInThenContinue;
   handler.refreshCredentialsThenContinue = refreshCredentialsThenContinue;
   handler.signIn = signInArg;
   if (!signOutUrl.empty())
   {
      handler.signOut = boost::bind(signOut, _1, _2, getUserIdentifierArg, signOutUrl);
   }
   handler.refreshAuthCookies = boost::bind(setSignInCookies, _1, _2, _3, _4, true);
}

} // namespace common
} // namespace auth
} // namespace server
} // namespace rstudio

#endif // SERVER_AUTH_COMMON_CPP
