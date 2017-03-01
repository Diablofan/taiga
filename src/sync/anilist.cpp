/*
** Taiga
** Copyright (C) 2010-2014, Eren Okka
** 
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "base/file.h"
#include "base/http.h"
#include "base/json.h"
#include "base/string.h"
#include "library/anime_db.h"
#include "library/anime_item.h"
#include "sync/anilist.h"
#include "sync/anilist_types.h"
#include "sync/anilist_util.h"
#include "taiga/settings.h"
#include "ui/ui.h"

namespace sync {
namespace anilist {

Service::Service() {
  host_ = L"anilist.co";
  // Todo: Fill these in
  client_id_ = L"";
  client_secret_ = L"";

  id_ = kAniList;
  canonical_name_ = L"anilist";
  name_ = L"AniList";
}

////////////////////////////////////////////////////////////////////////////////

void Service::BuildRequest(Request& request, HttpRequest& http_request) {
  http_request.url.host = host_;
  // HTTPS is required by the API. Users won't be redirected if they
  // use HTTP instead.
  http_request.url.protocol = base::http::kHttps;

  // AniList is supposed to return a JSON response for each and every
  // request, so that's what we expect from it.
  http_request.header[L"Accept"] = L"application/json";
  http_request.header[L"Accept-Charset"] = L"utf-8";
  http_request.header[L"Accept-Encoding"] = L"gzip";
  http_request.header[L"Content-Type"] = L"application/x-www-form-urlencoded";

  // kAuthenticateUser method returns the user's authentication token, which
  // is to be used on all methods that require authentication. Some methods
  // don't require the token, but behave differently when it's provided.
  if (RequestNeedsAuthentication(request.type))
    http_request.header[L"Authorization"] = L"Bearer " + auth_token_;

  switch (request.type) {
    BUILD_HTTP_REQUEST(kAuthenticateUser, AuthenticateUser);
    BUILD_HTTP_REQUEST(kRefreshAuth, RefreshAuth);
  }

  // APIv1 provides different title and alternate_title values depending on the
  // title_language_preference parameter.
  if (RequestSupportsTitleLanguagePreference(request.type))
    AppendTitleLanguagePreference(http_request);
}

void Service::HandleResponse(Response& response, HttpResponse& http_response) {
  if (RequestSucceeded(response, http_response)) {
    switch (response.type) {
      HANDLE_HTTP_RESPONSE(kAuthenticateUser, AuthenticateUser);
      HANDLE_HTTP_RESPONSE(kRefreshAuth, RefreshAuth);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Request builders

bool Service::RequestAniListPin(string_t& auth_pin) {
  string_t auth_url;
  auth_url = L"https://anilist.co/api/auth/authorize";
  auth_url += L"?response_type=pin&grant_type=authorization_pin";
  auth_url += L"&client_id=" + client_id_;
  ExecuteLink(auth_url);
  return ui::OnTokenEntry(auth_pin, L"AniList");
}

void Service::AuthenticateUser(Request& request, HttpRequest& http_request) {
  string_t auth_pin;
  if (!RequestAniListPin(auth_pin)) {
    return;
  }

  http_request.method = L"POST";
  http_request.header[L"Content-Type"] = L"application/x-www-form-urlencoded";
  http_request.url.path = L"/api/auth/access_token";

  http_request.data[L"grant_type"] = L"authorization_pin";
  http_request.data[L"client_id"] = client_id_;
  http_request.data[L"client_secret"] = client_secret_;
  http_request.data[L"code"] = auth_pin;
}

void Service::RefreshAuth(Request& request, HttpRequest& http_request) {
  http_request.method = L"POST";
  http_request.header[L"Content-Type"] = L"application/x-www-form-urlencoded";
  http_request.url.path = L"/api/auth/access_token";

  http_request.data[L"grant_type"] = L"refresh_token";
  http_request.data[L"client_id"] = client_id_;
  http_request.data[L"client_secret"] = client_secret_;
  http_request.data[L"refresh_token"] = refresh_token_;
}

////////////////////////////////////////////////////////////////////////////////
// Response handlers

void Service::AuthenticateUser(Response& response, HttpResponse& http_response) {
  Json::Reader reader;
  Json::Value root;
  reader.parse(WstrToStr(http_response.body), root);
  auth_token_ = StrToWstr(root["access_token"].asString());
  refresh_token_ = StrToWstr(root["refresh_token"].asString());
  // Kick off one hour timer to refresh.
}

void Service::RefreshAuth(Response& response, HttpResponse& http_response) {
  Json::Reader reader;
  Json::Value root;
  reader.parse(WstrToStr(http_response.body), root);
  auth_token_ = StrToWstr(root["access_token"].asString());
  // Kick off timer again.
}

////////////////////////////////////////////////////////////////////////////////

void Service::AppendTitleLanguagePreference(HttpRequest& http_request) const {
  // Can be "canonical", "english" or "romanized"
  std::wstring language = L"romanized";

  if (Settings.GetBool(taiga::kApp_List_DisplayEnglishTitles))
    language = L"english";

  if (http_request.method == L"POST") {
    http_request.data[L"title_language_preference"] = language;
  } else {
    http_request.url.query[L"title_language_preference"] = language;
  }
}

bool Service::RequestSupportsTitleLanguagePreference(RequestType request_type) const {
  switch (request_type) {
    case kGetLibraryEntries:
    case kGetMetadataById:
    case kSearchTitle:
    case kUpdateLibraryEntry:
      return true;
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////

bool Service::RequestNeedsAuthentication(RequestType request_type) const {
  switch (request_type) {
    case kAddLibraryEntry:
    case kDeleteLibraryEntry:
    case kUpdateLibraryEntry:
      return true;
    case kGetLibraryEntries:
    case kGetMetadataById:
    case kSearchTitle:
      return !auth_token_.empty();
  }

  return false;
}

bool Service::RequestSucceeded(Response& response,
                               const HttpResponse& http_response) {
  switch (http_response.code) {
    // OK
    case 200:
    case 201:
      return true;

    // Error
    default: {
      Json::Value root;
      Json::Reader reader;
      bool parsed = reader.parse(WstrToStr(http_response.body), root);
      response.data[L"error"] = name() + L" returned an error: ";
      if (parsed) {
        auto error = StrToWstr(root["error"].asString());
        response.data[L"error"] += error;
        if (response.type == kGetMetadataById) {
          if (InStr(error, L"Couldn't find Anime with 'id'=") > -1)
            response.data[L"invalid_id"] = L"true";
        }
      } else {
        response.data[L"error"] += L"Unknown error (" +
            canonical_name() + L"|" +
            ToWstr(response.type) + L"|" +
            ToWstr(http_response.code) + L")";
      }
      return false;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void Service::ParseAnimeObject(Json::Value& value, anime::Item& anime_item) {
  anime_item.SetSlug(StrToWstr(value["slug"].asString()));
  anime_item.SetAiringStatus(TranslateSeriesStatusFrom(StrToWstr(value["status"].asString())));
  anime_item.SetTitle(StrToWstr(value["title"].asString()));
  anime_item.SetSynonyms(StrToWstr(value["alternate_title"].asString()));
  anime_item.SetEpisodeCount(value["episode_count"].asInt());
  anime_item.SetEpisodeLength(value["episode_length"].asInt());
  anime_item.SetImageUrl(StrToWstr(value["cover_image"].asString()));
  anime_item.SetSynopsis(StrToWstr(value["synopsis"].asString()));
  anime_item.SetType(TranslateSeriesTypeFrom(StrToWstr(value["show_type"].asString())));
  anime_item.SetDateStart(StrToWstr(value["started_airing"].asString()));
  anime_item.SetDateEnd(StrToWstr(value["finished_airing"].asString()));
  anime_item.SetScore(TranslateSeriesRatingFrom(value["community_rating"].asFloat()));
  anime_item.SetAgeRating(TranslateAgeRatingFrom(StrToWstr(value["age_rating"].asString())));

  int mal_id = value["mal_id"].asInt();
  if (mal_id > 0)
    anime_item.SetId(ToWstr(mal_id), sync::kMyAnimeList);

  std::vector<std::wstring> genres;
  auto& genres_value = value["genres"];
  for (size_t i = 0; i < genres_value.size(); i++)
    genres.push_back(StrToWstr(genres_value[i]["name"].asString()));

  if (!genres.empty())
    anime_item.SetGenres(genres);
}

void Service::ParseLibraryObject(Json::Value& value) {
  auto& anime_value = value["anime"];
  auto& rating_value = value["rating"];

  ::anime::Item anime_item;
  anime_item.SetSource(this->id());
  anime_item.SetId(ToWstr(anime_value["id"].asInt()), this->id());
  anime_item.SetLastModified(time(nullptr));  // current time

  ParseAnimeObject(anime_value, anime_item);

  anime_item.AddtoUserList();
  anime_item.SetMyLastWatchedEpisode(value["episodes_watched"].asInt());
  anime_item.SetMyLastUpdated(TranslateMyLastUpdatedFrom(StrToWstr(value["updated_at"].asString())));
  anime_item.SetMyRewatchedTimes(value["rewatched_times"].asInt());
  anime_item.SetMyStatus(TranslateMyStatusFrom(StrToWstr(value["status"].asString())));
  anime_item.SetMyRewatching(value["rewatching"].asBool());
  anime_item.SetMyScore(TranslateMyRatingFrom(StrToWstr(rating_value["value"].asString()),
                                              StrToWstr(rating_value["type"].asString())));

  AnimeDatabase.UpdateItem(anime_item);
}

bool Service::ParseResponseBody(Response& response, HttpResponse& http_response,
                                Json::Value& root) {
  Json::Reader reader;

  if (http_response.body != L"false")
    if (reader.parse(WstrToStr(http_response.body), root))
      return true;

  switch (response.type) {
    case kGetLibraryEntries:
      response.data[L"error"] = L"Could not parse the list";
      break;
    case kGetMetadataById:
      response.data[L"error"] = L"Could not parse the anime object";
      break;
    case kSearchTitle:
      response.data[L"error"] = L"Could not parse search results";
      break;
    case kUpdateLibraryEntry:
      response.data[L"error"] = L"Could not parse library entry";
      break;
  }

  return false;
}

}  // namespace anilist
}  // namespace sync