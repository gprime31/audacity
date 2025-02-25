/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  UserService.cpp

  Dmitry Vedenko

**********************************************************************/

#include "UserService.h"

#include <memory>
#include <vector>

#include <wx/file.h>

#include <rapidjson/document.h>

#include "ServiceConfig.h"
#include "OAuthService.h"

#include "BasicUI.h"
#include "FileNames.h"
#include "Observer.h"
#include "Prefs.h"

#include "IResponse.h"
#include "NetworkManager.h"
#include "Request.h"

#include "CodeConversions.h"

namespace cloud::audiocom
{
namespace
{
wxString MakeAvatarPath()
{
   const wxFileName avatarFileName(FileNames::ConfigDir(), "avatar");
   return avatarFileName.GetFullPath();
}
   
StringSetting userName { L"/cloud/audiocom/userName", "" };
StringSetting displayName { L"/cloud/audiocom/displayName", "" };
StringSetting avatarEtag { L"/cloud/audiocom/avatarEtag", "" };

Observer::Subscription authStateChangedSubscription =
   GetOAuthService().Subscribe(
      [](const auto& state)
      {
         if (state.authorised)
            GetUserService().UpdateUserData();
         else
            GetUserService().ClearUserData();
      });

} // namespace

void UserService::UpdateUserData()
{
   auto& oauthService = GetOAuthService();

   if (!oauthService.HasAccessToken())
      return;

   using namespace audacity::network_manager;

   Request request(GetServiceConfig().GetAPIUrl("/me"));

   request.setHeader(
      common_headers::Authorization,
      std::string(oauthService.GetAccessToken()));

   request.setHeader(
      common_headers::Accept, common_content_types::ApplicationJson);

   auto response = NetworkManager::GetInstance().doGet(request);

   response->setRequestFinishedCallback(
      [response, this](auto)
      {
         const auto httpCode = response->getHTTPCode();
         
         if (httpCode != 200)
            return;

         const auto body = response->readAll<std::string>();

         using namespace rapidjson;

         Document document;
         document.Parse(body.data(), body.size());

         if (!document.IsObject())
            return;

         const auto username = document["username"].GetString();
         const auto avatar = document["avatar"].GetString();
         const auto profileName = document["profile"]["name"].GetString();

         BasicUI::CallAfter(
            [this, username = std::string(username),
             profileName = std::string(profileName),
             avatar = std::string(avatar)]()
            {
               userName.Write(username);
               displayName.Write(profileName);
               
               gPrefs->Flush();

               Publish({});

               DownloadAvatar(avatar);
            });
      });
}

void UserService::ClearUserData()
{
   BasicUI::CallAfter(
      [this]()
      {
         userName.Write({});
         displayName.Write({});
         avatarEtag.Write({});

         gPrefs->Flush();

         Publish({});
      });
}

UserService& GetUserService()
{
   static UserService userService;
   return userService;
}

void UserService::DownloadAvatar(std::string_view url)
{
   const auto avatarPath = MakeAvatarPath();
   const auto avatarTempPath = avatarPath + ".tmp";
   
   std::shared_ptr<wxFile> avatarFile = std::make_shared<wxFile>();

   if (!avatarFile->Create(avatarTempPath, true))
      return;
   
   using namespace audacity::network_manager;

   auto request = Request(std::string(url));

   const auto etag = audacity::ToUTF8(avatarEtag.Read());

   // If ETag is present - use it to prevent re-downloading the same file
   if (!etag.empty() && wxFileExists(avatarPath))
      request.setHeader(common_headers::IfNoneMatch, etag);

   auto response = NetworkManager::GetInstance().doGet(request);
   
   response->setOnDataReceivedCallback(
      [response, avatarFile](auto)
      {
         std::vector<char> buffer(response->getBytesAvailable());

         size_t bytes = response->readData(buffer.data(), buffer.size());

         avatarFile->Write(buffer.data(), buffer.size());
      });

   response->setRequestFinishedCallback(
      [response, avatarFile, avatarPath, avatarTempPath, this](auto)
      {
         avatarFile->Close();

         const auto httpCode = response->getHTTPCode();

         if (httpCode != 200)
         {
            // For any response excpept 200 just remove the temp file
            wxRemoveFile(avatarTempPath);
            return;
         }

         const auto etag = response->getHeader("ETag");
         const auto oldPath = avatarPath + ".old";
         
         if (wxFileExists(avatarPath))
            if (!wxRenameFile(avatarPath, oldPath))
               return;

         if (!wxRenameFile(avatarTempPath, avatarPath))
         {
            // Try at least to get it back...
            wxRenameFile(oldPath, avatarPath);
            return;
         }

         if (wxFileExists(oldPath))
            wxRemoveFile(oldPath);
         
         BasicUI::CallAfter(
            [this, etag]()
            {
               avatarEtag.Write(etag);
               gPrefs->Flush();
               
               Publish({});
            });
      });
}

wxString UserService::GetDisplayName() const
{
   return displayName.Read();
}

wxString UserService::GetUserSlug() const
{
   return userName.Read();
}

wxString UserService::GetAvatarPath() const
{
   auto path = MakeAvatarPath();

   if (!wxFileExists(path))
      return {};

   return path;
}

} // namespace cloud::audiocom
