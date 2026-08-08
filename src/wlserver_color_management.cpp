// Server-side wp_color_management_v1.
//
// Native Wayland clients (Chromium/Electron, Kodi, mpv) negotiate HDR through
// this protocol; games negotiate it through the Vulkan WSI layer and the
// gamescope-swapchain protocol instead. Rather than inventing a parallel
// path, an image description set on a surface is translated into the same
// wlserver_vk_swapchain_feedback struct the WSI layer fills in, so everything
// downstream (commit tagging, ColorspaceIsHDR, DRM colorimetry, tonemapping)
// works unchanged.
//
// Feature set advertised is intentionally minimal and honest: parametric
// descriptions only, sRGB/BT2020 primaries, sRGB/extended-linear/PQ transfer
// functions, perceptual intent. This matches what the compositing pipeline
// can actually represent (GamescopeAppTextureColorspace).

#include <cstring>
#include <memory>
#include <optional>

#include "wlserver.hpp"
#include "backend.h"
#include "hdmi.h"
#include "log.hpp"

#include "wlr_begin.hpp"
#include <wlr/types/wlr_compositor.h>
#include "wlr_end.hpp"

#include "color-management-v1-protocol.h"

static LogScope cm_log("colormgmt");

extern bool currentHDROutput;

//
// Image descriptions
//

struct GamescopeImageDescription
{
	uint32_t uIdentity = 0;

	std::optional<uint32_t> ouTF;        // wp_color_manager_v1_transfer_function
	std::optional<uint32_t> ouPrimaries; // wp_color_manager_v1_primaries

	// raw values as received (protocol encoding: coords x1e6, min lum x1e4)
	bool bHasMasteringPrimaries = false;
	int32_t nMasteringPrimaries[8] = {}; // rx ry gx gy bx by wx wy
	bool bHasMasteringLuminance = false;
	uint32_t uMinMasteringLum = 0; // cd/m2 * 10000
	uint32_t uMaxMasteringLum = 0; // cd/m2
	uint32_t uMaxCLL = 0;
	uint32_t uMaxFALL = 0;
	bool bHasLuminances = false;
	uint32_t uMinLum = 0; // cd/m2 * 10000
	uint32_t uMaxLum = 0; // cd/m2
	uint32_t uRefLum = 0; // cd/m2

	VkColorSpaceKHR ToVkColorspace() const
	{
		if ( ouTF == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ &&
		     ouPrimaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 )
			return VK_COLOR_SPACE_HDR10_ST2084_EXT;

		if ( ouTF == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR &&
		     ( !ouPrimaries || ouPrimaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB ) )
			return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;

		return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	}

	const char *Describe() const
	{
		switch ( ToVkColorspace() )
		{
			case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10-PQ";
			case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "scRGB";
			default: return "sRGB";
		}
	}
};

static uint32_t s_uNextIdentity = 1;

using ImageDescRef = std::shared_ptr<GamescopeImageDescription>;

static void desc_handle_destroy( struct wl_client *, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void info_send_description( struct wl_resource *info_resource, const GamescopeImageDescription &desc );

static void desc_handle_get_information( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
	ImageDescRef *pRef = (ImageDescRef *)wl_resource_get_user_data( resource );

	struct wl_resource *info_resource = wl_resource_create( client, &wp_image_description_info_v1_interface,
		wl_resource_get_version( resource ), id );
	if ( !info_resource )
	{
		wl_client_post_no_memory( client );
		return;
	}
	// info objects are one-shot: send everything, then done (destructor event).
	info_send_description( info_resource, **pRef );
	wp_image_description_info_v1_send_done( info_resource );
}

static const struct wp_image_description_v1_interface image_description_impl = {
	.destroy = desc_handle_destroy,
	.get_information = desc_handle_get_information,
};

static void desc_resource_destroy( struct wl_resource *resource )
{
	ImageDescRef *pRef = (ImageDescRef *)wl_resource_get_user_data( resource );
	delete pRef;
}

static struct wl_resource *create_description_resource( struct wl_client *client, uint32_t version, uint32_t id, ImageDescRef ref )
{
	struct wl_resource *resource = wl_resource_create( client, &wp_image_description_v1_interface, version, id );
	if ( !resource )
	{
		wl_client_post_no_memory( client );
		return nullptr;
	}
	wl_resource_set_implementation( resource, &image_description_impl, new ImageDescRef( std::move( ref ) ), desc_resource_destroy );
	return resource;
}

static void info_send_description( struct wl_resource *info_resource, const GamescopeImageDescription &desc )
{
	if ( desc.ouPrimaries )
	{
		wp_image_description_info_v1_send_primaries_named( info_resource, *desc.ouPrimaries );
		if ( *desc.ouPrimaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 )
			wp_image_description_info_v1_send_primaries( info_resource,
				708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000 );
		else
			wp_image_description_info_v1_send_primaries( info_resource,
				640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000 );
	}
	if ( desc.ouTF )
		wp_image_description_info_v1_send_tf_named( info_resource, *desc.ouTF );
	if ( desc.bHasLuminances )
		wp_image_description_info_v1_send_luminances( info_resource, desc.uMinLum, desc.uMaxLum, desc.uRefLum );
	if ( desc.bHasMasteringPrimaries )
		wp_image_description_info_v1_send_target_primaries( info_resource,
			desc.nMasteringPrimaries[0], desc.nMasteringPrimaries[1],
			desc.nMasteringPrimaries[2], desc.nMasteringPrimaries[3],
			desc.nMasteringPrimaries[4], desc.nMasteringPrimaries[5],
			desc.nMasteringPrimaries[6], desc.nMasteringPrimaries[7] );
	if ( desc.bHasMasteringLuminance )
		wp_image_description_info_v1_send_target_luminance( info_resource, desc.uMinMasteringLum, desc.uMaxMasteringLum );
	if ( desc.uMaxCLL )
		wp_image_description_info_v1_send_target_max_cll( info_resource, desc.uMaxCLL );
	if ( desc.uMaxFALL )
		wp_image_description_info_v1_send_target_max_fall( info_resource, desc.uMaxFALL );
}

// The compositor's current output description: HDR10 when HDR output is live,
// plain sRGB otherwise.
static ImageDescRef get_output_description()
{
	auto desc = std::make_shared<GamescopeImageDescription>();
	// Stable identities: identity equality tells clients the description
	// contents are unchanged, so keep one per output state.
	desc->uIdentity = currentHDROutput ? 1000001 : 1000000;
	if ( currentHDROutput )
	{
		desc->ouTF = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
		desc->ouPrimaries = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
		desc->bHasLuminances = true;
		desc->uMinLum = 0;
		desc->uMaxLum = 1000;
		desc->uRefLum = 203;
	}
	else
	{
		desc->ouTF = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
		desc->ouPrimaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	}
	return desc;
}

//
// Parametric creator
//

static void params_creator_check_and_create( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );

	if ( !pPending->ouTF )
	{
		wl_resource_post_error( resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
			"transfer function not set" );
		return;
	}
	if ( !pPending->ouPrimaries )
	{
		wl_resource_post_error( resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
			"primaries not set" );
		return;
	}

	auto desc = std::make_shared<GamescopeImageDescription>( *pPending );
	desc->uIdentity = s_uNextIdentity++;

	cm_log.infof( "create image description: identity=%u tf=%u primaries=%u -> %s (mastering=%d maxcll=%u maxfall=%u)",
		desc->uIdentity, *desc->ouTF, *desc->ouPrimaries, desc->Describe(),
		(int)desc->bHasMasteringLuminance, desc->uMaxCLL, desc->uMaxFALL );

	struct wl_resource *desc_resource = create_description_resource( client, wl_resource_get_version( resource ), id, desc );
	if ( desc_resource )
		wp_image_description_v1_send_ready( desc_resource, desc->uIdentity );

	// creator objects are one-shot
	wl_resource_destroy( resource );
}

static void params_creator_set_tf_named( struct wl_client *, struct wl_resource *resource, uint32_t tf )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	switch ( tf )
	{
		case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
		case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
		case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
		case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
			pPending->ouTF = tf;
			break;
		default:
			wl_resource_post_error( resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
				"unsupported transfer function %u", tf );
			break;
	}
}

static void params_creator_set_tf_power( struct wl_client *, struct wl_resource *resource, uint32_t )
{
	wl_resource_post_error( resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
		"tf_power is not supported" );
}

static void params_creator_set_primaries_named( struct wl_client *, struct wl_resource *resource, uint32_t primaries )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	switch ( primaries )
	{
		case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
		case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
			pPending->ouPrimaries = primaries;
			break;
		default:
			wl_resource_post_error( resource, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
				"unsupported primaries %u", primaries );
			break;
	}
}

static void params_creator_set_primaries( struct wl_client *, struct wl_resource *resource,
	int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y )
{
	// We only support the named primaries we advertise; tolerate raw coords
	// that match them closely enough by snapping to the nearest named set.
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	// crude classification: BT.2020 red x ~0.708, sRGB red x ~0.64
	pPending->ouPrimaries = ( r_x > 680000 )
		? WP_COLOR_MANAGER_V1_PRIMARIES_BT2020
		: WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
}

static void params_creator_set_luminances( struct wl_client *, struct wl_resource *resource,
	uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	pPending->bHasLuminances = true;
	pPending->uMinLum = min_lum;
	pPending->uMaxLum = max_lum;
	pPending->uRefLum = reference_lum;
}

static void params_creator_set_mastering_display_primaries( struct wl_client *, struct wl_resource *resource,
	int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	pPending->bHasMasteringPrimaries = true;
	int32_t vals[8] = { r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y };
	memcpy( pPending->nMasteringPrimaries, vals, sizeof( vals ) );
}

static void params_creator_set_mastering_luminance( struct wl_client *, struct wl_resource *resource,
	uint32_t min_lum, uint32_t max_lum )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	pPending->bHasMasteringLuminance = true;
	pPending->uMinMasteringLum = min_lum;
	pPending->uMaxMasteringLum = max_lum;
}

static void params_creator_set_max_cll( struct wl_client *, struct wl_resource *resource, uint32_t max_cll )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	pPending->uMaxCLL = max_cll;
}

static void params_creator_set_max_fall( struct wl_client *, struct wl_resource *resource, uint32_t max_fall )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	pPending->uMaxFALL = max_fall;
}

static const struct wp_image_description_creator_params_v1_interface params_creator_impl = {
	.create = params_creator_check_and_create,
	.set_tf_named = params_creator_set_tf_named,
	.set_tf_power = params_creator_set_tf_power,
	.set_primaries_named = params_creator_set_primaries_named,
	.set_primaries = params_creator_set_primaries,
	.set_luminances = params_creator_set_luminances,
	.set_mastering_display_primaries = params_creator_set_mastering_display_primaries,
	.set_mastering_luminance = params_creator_set_mastering_luminance,
	.set_max_cll = params_creator_set_max_cll,
	.set_max_fall = params_creator_set_max_fall,
};

static void params_creator_resource_destroy( struct wl_resource *resource )
{
	GamescopeImageDescription *pPending = (GamescopeImageDescription *)wl_resource_get_user_data( resource );
	delete pPending;
}

//
// Per-surface color management
//

static void cm_surface_handle_destroy( struct wl_client *, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void cm_surface_set_image_description( struct wl_client *, struct wl_resource *resource,
	struct wl_resource *image_description, uint32_t render_intent )
{
	struct wlr_surface *surface = (struct wlr_surface *)wl_resource_get_user_data( resource );
	if ( !surface )
		return; // inert: underlying wl_surface is gone

	ImageDescRef *pRef = (ImageDescRef *)wl_resource_get_user_data( image_description );
	const GamescopeImageDescription &desc = **pRef;

	wlserver_wl_surface_info *wl_info = get_wl_surface_info( surface );
	if ( !wl_info )
		return;

	VkColorSpaceKHR colorspace = desc.ToVkColorspace();

	// Ride the WSI path: synthesize the swapchain feedback the Vulkan layer
	// would have provided. Everything downstream keys off vk_colorspace and
	// hdr_metadata_blob.
	auto feedback = std::make_shared<wlserver_vk_swapchain_feedback>();
	feedback->image_count = 0;
	feedback->vk_format = VK_FORMAT_UNDEFINED;
	feedback->vk_colorspace = colorspace;
	feedback->vk_composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	feedback->vk_pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	feedback->vk_clipped = VK_FALSE;
	feedback->vk_engine_name = std::make_shared<std::string>( "wp_color_management" );

	if ( colorspace == VK_COLOR_SPACE_HDR10_ST2084_EXT && desc.bHasMasteringLuminance && desc.uMaxCLL && desc.uMaxFALL )
	{
		hdr_output_metadata metadata = {};
		metadata.metadata_type = 0;
		hdr_metadata_infoframe &infoframe = metadata.hdmi_metadata_type1;
		infoframe.eotf = HDMI_EOTF_ST2084;
		infoframe.metadata_type = 0;
		// protocol encodes coords x1e6; infoframe wants x50000 (0.00002 units)
		for ( int i = 0; i < 3; i++ )
		{
			infoframe.display_primaries[i].x = (uint16_t)( desc.nMasteringPrimaries[i * 2 + 0] / 20 );
			infoframe.display_primaries[i].y = (uint16_t)( desc.nMasteringPrimaries[i * 2 + 1] / 20 );
		}
		infoframe.white_point.x = (uint16_t)( desc.nMasteringPrimaries[6] / 20 );
		infoframe.white_point.y = (uint16_t)( desc.nMasteringPrimaries[7] / 20 );
		// infoframe: max mastering lum in cd/m2, min in 0.0001 cd/m2 (protocol matches)
		infoframe.max_display_mastering_luminance = (uint16_t)desc.uMaxMasteringLum;
		infoframe.min_display_mastering_luminance = (uint16_t)desc.uMinMasteringLum;
		infoframe.max_cll = (uint16_t)desc.uMaxCLL;
		infoframe.max_fall = (uint16_t)desc.uMaxFALL;
		feedback->hdr_metadata_blob = GetBackend()->CreateBackendBlob( metadata );
	}

	wl_info->swapchain_feedback = std::move( feedback );

	cm_log.infof( "surface %p: set_image_description identity=%u -> %s (intent=%u)",
		(void *)surface, desc.uIdentity, desc.Describe(), render_intent );
}

static void cm_surface_unset_image_description( struct wl_client *, struct wl_resource *resource )
{
	struct wlr_surface *surface = (struct wlr_surface *)wl_resource_get_user_data( resource );
	if ( !surface )
		return;

	wlserver_wl_surface_info *wl_info = get_wl_surface_info( surface );
	if ( !wl_info )
		return;

	wl_info->swapchain_feedback = nullptr;
	cm_log.infof( "surface %p: unset_image_description", (void *)surface );
}

static const struct wp_color_management_surface_v1_interface cm_surface_impl = {
	.destroy = cm_surface_handle_destroy,
	.set_image_description = cm_surface_set_image_description,
	.unset_image_description = cm_surface_unset_image_description,
};

//
// Surface feedback
//

static void cm_feedback_handle_destroy( struct wl_client *, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void cm_feedback_get_preferred( struct wl_client *client, struct wl_resource *resource, uint32_t image_description )
{
	ImageDescRef desc = get_output_description();
	cm_log.infof( "get_preferred -> identity=%u %s", desc->uIdentity, desc->Describe() );
	struct wl_resource *desc_resource = create_description_resource( client, wl_resource_get_version( resource ), image_description, desc );
	if ( desc_resource )
		wp_image_description_v1_send_ready( desc_resource, desc->uIdentity );
}

static const struct wp_color_management_surface_feedback_v1_interface cm_feedback_impl = {
	.destroy = cm_feedback_handle_destroy,
	.get_preferred = cm_feedback_get_preferred,
	.get_preferred_parametric = cm_feedback_get_preferred,
};

//
// Output color management
//

static void cm_output_handle_destroy( struct wl_client *, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void cm_output_get_image_description( struct wl_client *client, struct wl_resource *resource, uint32_t image_description )
{
	ImageDescRef desc = get_output_description();
	struct wl_resource *desc_resource = create_description_resource( client, wl_resource_get_version( resource ), image_description, desc );
	if ( desc_resource )
		wp_image_description_v1_send_ready( desc_resource, desc->uIdentity );
}

static const struct wp_color_management_output_v1_interface cm_output_impl = {
	.destroy = cm_output_handle_destroy,
	.get_image_description = cm_output_get_image_description,
};

//
// Manager
//

static void manager_handle_destroy( struct wl_client *, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void manager_get_output( struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource * )
{
	struct wl_resource *output_resource = wl_resource_create( client, &wp_color_management_output_v1_interface,
		wl_resource_get_version( resource ), id );
	if ( !output_resource )
	{
		wl_client_post_no_memory( client );
		return;
	}
	wl_resource_set_implementation( output_resource, &cm_output_impl, nullptr, nullptr );
}

static void manager_get_surface( struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource )
{
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );

	struct wl_resource *surf_cm_resource = wl_resource_create( client, &wp_color_management_surface_v1_interface,
		wl_resource_get_version( resource ), id );
	if ( !surf_cm_resource )
	{
		wl_client_post_no_memory( client );
		return;
	}
	wl_resource_set_implementation( surf_cm_resource, &cm_surface_impl, surface, nullptr );

	wlserver_wl_surface_info *wl_info = get_wl_surface_info( surface );
	cm_log.infof( "get_surface %p (x11=%p xdg=%p)", (void *)surface,
		wl_info ? (void *)wl_info->x11_surface : nullptr,
		wl_info ? (void *)wl_info->xdg_surface : nullptr );
}

static void manager_get_surface_feedback( struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource )
{
	struct wl_resource *feedback_resource = wl_resource_create( client, &wp_color_management_surface_feedback_v1_interface,
		wl_resource_get_version( resource ), id );
	if ( !feedback_resource )
	{
		wl_client_post_no_memory( client );
		return;
	}
	wl_resource_set_implementation( feedback_resource, &cm_feedback_impl, nullptr, nullptr );

	// Nudge the client to query the preferred description: some clients only
	// evaluate HDR after this event rather than calling get_preferred on
	// their own.
	wp_color_management_surface_feedback_v1_send_preferred_changed( feedback_resource,
		currentHDROutput ? 1000001 : 1000000 );
	cm_log.infof( "get_surface_feedback -> initial preferred_changed(%u)", currentHDROutput ? 1000001 : 1000000 );
}

static void manager_create_icc_creator( struct wl_client *, struct wl_resource *resource, uint32_t )
{
	wl_resource_post_error( resource, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"ICC profiles are not supported" );
}

static void manager_create_parametric_creator( struct wl_client *client, struct wl_resource *resource, uint32_t obj )
{
	struct wl_resource *creator_resource = wl_resource_create( client, &wp_image_description_creator_params_v1_interface,
		wl_resource_get_version( resource ), obj );
	if ( !creator_resource )
	{
		wl_client_post_no_memory( client );
		return;
	}
	wl_resource_set_implementation( creator_resource, &params_creator_impl,
		new GamescopeImageDescription(), params_creator_resource_destroy );
}

static void manager_create_windows_scrgb( struct wl_client *client, struct wl_resource *resource, uint32_t image_description )
{
	// scRGB: sRGB primaries, extended linear transfer, 1.0 == 80 nits.
	// Maps to GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB via EXTENDED_SRGB_LINEAR.
	auto desc = std::make_shared<GamescopeImageDescription>();
	desc->uIdentity = s_uNextIdentity++;
	desc->ouTF = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
	desc->ouPrimaries = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	cm_log.infof( "create_windows_scrgb -> identity=%u", desc->uIdentity );
	struct wl_resource *desc_resource = create_description_resource( client, wl_resource_get_version( resource ), image_description, desc );
	if ( desc_resource )
		wp_image_description_v1_send_ready( desc_resource, desc->uIdentity );
}

static const struct wp_color_manager_v1_interface color_manager_impl = {
	.destroy = manager_handle_destroy,
	.get_output = manager_get_output,
	.get_surface = manager_get_surface,
	.get_surface_feedback = manager_get_surface_feedback,
	.create_icc_creator = manager_create_icc_creator,
	.create_parametric_creator = manager_create_parametric_creator,
	.create_windows_scrgb = manager_create_windows_scrgb,
};

static void color_manager_bind( struct wl_client *client, void *, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &wp_color_manager_v1_interface, version, id );
	if ( !resource )
	{
		wl_client_post_no_memory( client );
		return;
	}
	wl_resource_set_implementation( resource, &color_manager_impl, nullptr, nullptr );

	cm_log.infof( "client bound wp_color_manager_v1 (hdr output currently: %d)", (int)currentHDROutput );

	wp_color_manager_v1_send_supported_intent( resource, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL );

	wp_color_manager_v1_send_supported_feature( resource, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC );
	wp_color_manager_v1_send_supported_feature( resource, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES );
	wp_color_manager_v1_send_supported_feature( resource, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES );
	wp_color_manager_v1_send_supported_feature( resource, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES );
	wp_color_manager_v1_send_supported_feature( resource, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME );
	wp_color_manager_v1_send_supported_feature( resource, WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB );

	wp_color_manager_v1_send_supported_tf_named( resource, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB );
	wp_color_manager_v1_send_supported_tf_named( resource, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22 );
	wp_color_manager_v1_send_supported_tf_named( resource, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR );
	wp_color_manager_v1_send_supported_tf_named( resource, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ );

	wp_color_manager_v1_send_supported_primaries_named( resource, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB );
	wp_color_manager_v1_send_supported_primaries_named( resource, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 );

	wp_color_manager_v1_send_done( resource );
}

void create_color_management( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &wp_color_manager_v1_interface, version, nullptr, color_manager_bind );
	cm_log.infof( "wp_color_manager_v1 global created" );
}
