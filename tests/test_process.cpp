#include <catch2/catch_test_macros.hpp>

#include <stdlib.h>

#include <string>

#include "Utils/Process.h"

// Keep in sync with k_pszStashedOverlayPreload in Process.cpp.
static constexpr const char *k_pszStashedOverlayPreload = "GAMESCOPE_STEAM_OVERLAY_LD_PRELOAD";

static const char *k_pszOverlay = "/home/user/.local/share/Steam/ubuntu12_64/gameoverlayrenderer.so";

static std::string GetEnv( const char *pszName )
{
	const char *pszValue = getenv( pszName );
	REQUIRE( pszValue != nullptr );
	return pszValue;
}

TEST_CASE("RemoveSteamOverlayFromPreload", "[process]") {
	SECTION("does nothing when LD_PRELOAD is unset") {
		unsetenv( "LD_PRELOAD" );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( getenv( "LD_PRELOAD" ) == nullptr );
	}

	SECTION("unsets LD_PRELOAD when the overlay is the only entry") {
		setenv( "LD_PRELOAD", k_pszOverlay, 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( getenv( "LD_PRELOAD" ) == nullptr );
	}

	SECTION("removes the overlay and keeps the other entries") {
		setenv( "LD_PRELOAD", ( std::string{ "libfirst.so:" } + k_pszOverlay + ":liblast.so" ).c_str(), 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so:liblast.so" );
	}

	SECTION("handles space-separated lists") {
		setenv( "LD_PRELOAD", ( std::string{ "libfirst.so " } + k_pszOverlay ).c_str(), 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so" );
	}

	SECTION("leaves the other entries alone when the overlay is absent") {
		setenv( "LD_PRELOAD", "libfirst.so:liblast.so", 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so:liblast.so" );
	}
}

TEST_CASE("RestoreSteamOverlayPreload", "[process]") {
	SECTION("reports when there is nothing stashed") {
		unsetenv( k_pszStashedOverlayPreload );
		setenv( "LD_PRELOAD", "libuntouched.so", 1 );
		REQUIRE( gamescope::Process::RestoreSteamOverlayPreload() == false );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libuntouched.so" );
	}

	SECTION("puts the stashed preload back and consumes the stash") {
		setenv( k_pszStashedOverlayPreload, k_pszOverlay, 1 );
		unsetenv( "LD_PRELOAD" );
		REQUIRE( gamescope::Process::RestoreSteamOverlayPreload() == true );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == k_pszOverlay );
		REQUIRE( getenv( k_pszStashedOverlayPreload ) == nullptr );
	}
}
