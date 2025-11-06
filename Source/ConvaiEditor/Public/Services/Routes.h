/**
 * Copyright Convai Inc. All Rights Reserved.
 *
 * Routes.h
 *
 * Navigation routes for main UI pages.
 */

#pragma once

#include "CoreMinimal.h"

namespace ConvaiEditor
{
	namespace Route
	{
		/**
		 * Navigation routes.
		 */
		enum class E
		{
			/** No route selected */
			None,
			/** Home page */
			Home,
			/** Dashboard page with integrated web browser */
			Dashboard,
			/** Experiences page with integrated web browser */
			Experiences,
			/** YouTube video page with integrated web browser */
			YouTubeVideo,
			/** Documentation page with integrated web browser */
			Documentation,
			/** Forum page with integrated web browser */
			Forum,
			/** Samples page */
			Samples,
			/** Account page */
			Account,
			/** Support page */
			Support,
			/** Settings page with theme configuration */
			Settings
		};

		/** Converts route to string */
		inline FString ToString(E Route)
		{
			switch (Route)
			{
			case E::None:
				return TEXT("None");
			case E::Home:
				return TEXT("Home");
			case E::Dashboard:
				return TEXT("Dashboard");
			case E::Experiences:
				return TEXT("Experiences");
			case E::YouTubeVideo:
				return TEXT("YouTubeVideo");
			case E::Documentation:
				return TEXT("Documentation");
			case E::Forum:
				return TEXT("Forum");
			case E::Samples:
				return TEXT("Samples");
			case E::Account:
				return TEXT("Account");
			case E::Support:
				return TEXT("Support");
			case E::Settings:
				return TEXT("Settings");
			default:
				return TEXT("Unknown");
			}
		}

		/** Converts string to route */
		inline E FromString(const FString &RouteString)
		{
			if (RouteString == TEXT("None"))
				return E::None;
			if (RouteString == TEXT("Home"))
				return E::Home;
			if (RouteString == TEXT("Dashboard"))
				return E::Dashboard;
			if (RouteString == TEXT("Experiences"))
				return E::Experiences;
			if (RouteString == TEXT("YouTubeVideo"))
				return E::YouTubeVideo;
			if (RouteString == TEXT("Documentation"))
				return E::Documentation;
			if (RouteString == TEXT("Forum"))
				return E::Forum;
			if (RouteString == TEXT("Samples"))
				return E::Samples;
			if (RouteString == TEXT("Account"))
				return E::Account;
			if (RouteString == TEXT("Support"))
				return E::Support;
			if (RouteString == TEXT("Settings"))
				return E::Settings;
			return E::None; // Default to None instead of Home
		}
	}
} // namespace ConvaiEditor
