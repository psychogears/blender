/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup wm
 *
 * \name Window-Manager XR API
 *
 * Implements Blender specific functionality for the GHOST_Xr API.
 */

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_screen.h"

#include "BLI_ghash.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"

#include "DNA_camera_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"
#include "DNA_xr_types.h"

#include "DRW_engine.h"

#include "ED_view3d.h"
#include "ED_view3d_offscreen.h"

#include "GHOST_C-api.h"

#include "GPU_context.h"
#include "GPU_draw.h"
#include "GPU_matrix.h"
#include "GPU_viewport.h"

#include "MEM_guardedalloc.h"

#include "UI_interface.h"

#include "WM_types.h"
#include "WM_api.h"

#include "wm.h"
#include "wm_surface.h"
#include "wm_window.h"

void wm_xr_runtime_session_state_free(struct bXrRuntimeSessionState **state);
void wm_xr_draw_view(const GHOST_XrDrawViewInfo *, void *);
void *wm_xr_session_gpu_binding_context_create(GHOST_TXrGraphicsBinding);
void wm_xr_session_gpu_binding_context_destroy(GHOST_TXrGraphicsBinding, void *);
wmSurface *wm_xr_session_surface_create(wmWindowManager *, unsigned int);
void wm_xr_pose_to_viewmat(const GHOST_XrPose *pose, float r_viewmat[4][4]);

/* -------------------------------------------------------------------- */

typedef struct bXrRuntimeSessionState {
  /** The pose (location + rotation) to which eye deltas will be applied to when drawing (world
   * space). With positional tracking enabled, it should be the same as the base pose, when
   * disabled it also contains a location delta from the moment the option was toggled. */
  GHOST_XrPose final_reference_pose;
  float eye_position_ofs[3]; /* Local/view space. */

  /** Copy of bXrSessionSettings.flag created on the last draw call,  */
  int prev_settings_flag;

  /* Just a utility "cache" to avoid recalculating things for external queries. */
  struct {
    /** Last known viewer location (centroid of eyes, in world space) stored for queries. */
    GHOST_XrPose viewer_pose;
    /** The view matrix calculated from above's viewer pose. */
    float viewer_viewmat[4][4];
    float focal_len;

    bool is_initialized;
  } last_known;
} bXrRuntimeSessionState;

typedef struct {
  GHOST_TXrGraphicsBinding gpu_binding_type;
  GPUOffScreen *offscreen;
  GPUViewport *viewport;

  GHOST_ContextHandle secondary_ghost_ctx;
} wmXrSurfaceData;

typedef struct {
  wmWindowManager *wm;
  bContext *evil_C;
} wmXrErrorHandlerData;

/* -------------------------------------------------------------------- */

static wmSurface *g_xr_surface = NULL;

/* -------------------------------------------------------------------- */
/** \name XR-Context
 *
 * All XR functionality is accessed through a #GHOST_XrContext handle.
 * The lifetime of this context also determines the lifetime of the OpenXR instance, which is the
 * representation of the OpenXR runtime connection within the application.
 *
 * \{ */

static void wm_xr_error_handler(const GHOST_XrError *error)
{
  wmXrErrorHandlerData *handler_data = error->customdata;
  wmWindowManager *wm = handler_data->wm;

  BKE_reports_clear(&wm->reports);
  WM_report(RPT_ERROR, error->user_message);
  WM_report_banner_show();
  UI_popup_menu_reports(handler_data->evil_C, &wm->reports);

  if (wm->xr.context) {
    /* Just play safe and destroy the entire context. */
    GHOST_XrContextDestroy(wm->xr.context);
    wm->xr.context = NULL;
  }
}

bool wm_xr_context_ensure(bContext *C, wmWindowManager *wm)
{
  if (wm->xr.context) {
    return true;
  }
  static wmXrErrorHandlerData error_customdata;

  /* Set up error handling */
  error_customdata.wm = wm;
  error_customdata.evil_C = C;
  GHOST_XrErrorHandler(wm_xr_error_handler, &error_customdata);

  {
    const GHOST_TXrGraphicsBinding gpu_bindings_candidates[] = {
        GHOST_kXrGraphicsOpenGL,
#ifdef WIN32
        GHOST_kXrGraphicsD3D11,
#endif
    };
    GHOST_XrContextCreateInfo create_info = {
        .gpu_binding_candidates = gpu_bindings_candidates,
        .gpu_binding_candidates_count = ARRAY_SIZE(gpu_bindings_candidates)};

    if (G.debug & G_DEBUG_XR) {
      create_info.context_flag |= GHOST_kXrContextDebug;
    }
    if (G.debug & G_DEBUG_XR_TIME) {
      create_info.context_flag |= GHOST_kXrContextDebugTime;
    }

    if (!(wm->xr.context = GHOST_XrContextCreate(&create_info))) {
      return false;
    }

    /* Set up context callbacks */
    GHOST_XrGraphicsContextBindFuncs(wm->xr.context,
                                     wm_xr_session_gpu_binding_context_create,
                                     wm_xr_session_gpu_binding_context_destroy);
    GHOST_XrDrawViewFunc(wm->xr.context, wm_xr_draw_view);
  }
  BLI_assert(wm->xr.context != NULL);

  return true;
}

void wm_xr_data_destroy(wmWindowManager *wm)
{
  if (wm->xr.context != NULL) {
    GHOST_XrContextDestroy(wm->xr.context);
    wm->xr.context = NULL;
  }
  if (wm->xr.session_state != NULL) {
    wm_xr_runtime_session_state_free(&wm->xr.session_state);
  }
}

/** \} */ /* XR-Context */

/* -------------------------------------------------------------------- */
/** \name XR Runtime Session State
 *
 * \{ */

static bXrRuntimeSessionState *wm_xr_runtime_session_state_create(void)
{
  bXrRuntimeSessionState *state = MEM_callocN(sizeof(*state), __func__);
  return state;
}

void wm_xr_runtime_session_state_free(bXrRuntimeSessionState **state)
{
  MEM_SAFE_FREE(*state);
}

static void wm_xr_reference_pose_calc(const Scene *scene,
                                      const bXrSessionSettings *settings,
                                      GHOST_XrPose *r_pose)
{
  const Object *base_pose_object = ((settings->base_pose_type == XR_BASE_POSE_OBJECT) &&
                                    settings->base_pose_object) ?
                                       settings->base_pose_object :
                                       scene->camera;

  if (settings->base_pose_type == XR_BASE_POSE_CUSTOM) {
    float tmp_quatx[4], tmp_quatz[4];

    copy_v3_v3(r_pose->position, settings->base_pose_location);
    axis_angle_to_quat_single(tmp_quatx, 'X', M_PI_2);
    axis_angle_to_quat_single(tmp_quatz, 'Z', settings->base_pose_angle);
    mul_qt_qtqt(r_pose->orientation_quat, tmp_quatz, tmp_quatx);
  }
  else if (base_pose_object) {
    float tmp_quat[4];
    float tmp_eul[3];

    mat4_to_loc_quat(r_pose->position, tmp_quat, base_pose_object->obmat);

    /* Only use rotation around Z-axis to align view with floor. */
    quat_to_eul(tmp_eul, tmp_quat);
    tmp_eul[0] = M_PI_2;
    tmp_eul[1] = 0;
    eul_to_quat(r_pose->orientation_quat, tmp_eul);
  }
  else {
    copy_v3_fl(r_pose->position, 0.0f);
    unit_qt(r_pose->orientation_quat);
  }
}

static void wm_xr_runtime_session_state_final_reference_pose_update(
    bXrRuntimeSessionState *state,
    const GHOST_XrDrawViewInfo *draw_view,
    const bXrSessionSettings *settings,
    const Scene *scene)
{
  const bool position_tracking_toggled = (state->prev_settings_flag &
                                          XR_SESSION_USE_POSITION_TRACKING) !=
                                         (settings->flag & XR_SESSION_USE_POSITION_TRACKING);
  const bool use_position_tracking = settings->flag & XR_SESSION_USE_POSITION_TRACKING;

  wm_xr_reference_pose_calc(scene, settings, &state->final_reference_pose);

  if (position_tracking_toggled) {
    if (use_position_tracking) {
      copy_v3_fl(state->eye_position_ofs, 0.0f);
    }
    else {
      /* Store the current local offset (local pose) so that we can apply that to the eyes. This
       * way the eyes stay exactly where they are when disabling positional tracking. */
      copy_v3_v3(state->eye_position_ofs, draw_view->local_pose.position);
    }
  }

  state->prev_settings_flag = settings->flag;
}

static void wm_xr_runtime_session_state_info_update(bXrRuntimeSessionState *state,
                                                    const GHOST_XrDrawViewInfo *draw_view,
                                                    const bXrSessionSettings *settings)
{
  GHOST_XrPose viewer_pose;
  const bool use_position_tracking = settings->flag & XR_SESSION_USE_POSITION_TRACKING;

  mul_qt_qtqt(viewer_pose.orientation_quat,
              state->final_reference_pose.orientation_quat,
              draw_view->local_pose.orientation_quat);
  copy_v3_v3(viewer_pose.position, state->final_reference_pose.position);
  viewer_pose.position[0] += state->eye_position_ofs[0];
  viewer_pose.position[1] -= state->eye_position_ofs[2];
  viewer_pose.position[2] += state->eye_position_ofs[1];
  if (use_position_tracking) {
    viewer_pose.position[0] += draw_view->local_pose.position[0];
    viewer_pose.position[1] -= draw_view->local_pose.position[2];
    viewer_pose.position[2] += draw_view->local_pose.position[1];
  }

  state->last_known.viewer_pose = viewer_pose;
  wm_xr_pose_to_viewmat(&viewer_pose, state->last_known.viewer_viewmat);
  /* No idea why, but multiplying by two seems to make it match the VR view more. */
  state->last_known.focal_len = 2.0f * fov_to_focallength(draw_view->fov.angle_right -
                                                              draw_view->fov.angle_left,
                                                          DEFAULT_SENSOR_WIDTH);
  state->last_known.is_initialized = true;
}

void WM_xr_session_state_viewer_location_get(const wmXrData *xr, float r_location[3])
{
  if (!WM_xr_session_is_running(xr) || !xr->session_state->last_known.is_initialized) {
    return;
  }

  copy_v3_v3(r_location, xr->session_state->last_known.viewer_pose.position);
}

void WM_xr_session_state_viewer_rotation_get(const wmXrData *xr, float r_rotation[4])
{
  if (!WM_xr_session_is_running(xr) || !xr->session_state->last_known.is_initialized) {
    return;
  }

  copy_v4_v4(r_rotation, xr->session_state->last_known.viewer_pose.orientation_quat);
}

void WM_xr_session_state_viewer_matrix_info_get(const wmXrData *xr,
                                                float r_viewmat[4][4],
                                                float *r_focal_len)
{
  if (!WM_xr_session_is_running(xr) || !xr->session_state->last_known.is_initialized) {
    return;
  }

  copy_m4_m4(r_viewmat, xr->session_state->last_known.viewer_viewmat);
  *r_focal_len = xr->session_state->last_known.focal_len;
}

/** \} */ /* XR Runtime Session State */

/* -------------------------------------------------------------------- */
/** \name XR-Session
 *
 * \{ */

void *wm_xr_session_gpu_binding_context_create(GHOST_TXrGraphicsBinding graphics_binding)
{
  wmSurface *surface = wm_xr_session_surface_create(G_MAIN->wm.first, graphics_binding);
  wmXrSurfaceData *data = surface->customdata;

  wm_surface_add(surface);

  /* Some regions may need to redraw with updated session state after the session is entirely up
   * and running. */
  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);

  return data->secondary_ghost_ctx ? data->secondary_ghost_ctx : surface->ghost_ctx;
}

void wm_xr_session_gpu_binding_context_destroy(GHOST_TXrGraphicsBinding UNUSED(graphics_lib),
                                               void *UNUSED(context))
{
  if (g_xr_surface) { /* Might have been freed already */
    wm_surface_remove(g_xr_surface);
  }

  wm_window_reset_drawable();

  /* Some regions may need to redraw with updated session state after the session is entirely
   * stopped. */
  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);
}

static void wm_xr_session_begin_info_create(const bXrRuntimeSessionState *UNUSED(state),
                                            GHOST_XrSessionBeginInfo *UNUSED(r_begin_info))
{
}

void wm_xr_session_toggle(bContext *C, void *xr_context_ptr)
{
  GHOST_XrContextHandle xr_context = xr_context_ptr;
  wmWindowManager *wm = CTX_wm_manager(C);

  if (WM_xr_session_is_running(&wm->xr)) {
    GHOST_XrSessionEnd(xr_context);
    wm_xr_runtime_session_state_free(&wm->xr.session_state);
  }
  else {
    GHOST_XrSessionBeginInfo begin_info;

    wm->xr.session_state = wm_xr_runtime_session_state_create();
    wm_xr_session_begin_info_create(wm->xr.session_state, &begin_info);

    GHOST_XrSessionStart(xr_context, &begin_info);
  }
}

/**
 * The definition used here to define a session as running differs slightly from the OpenXR
 * sepecification one: Here we already consider a session as stopped when session-end request was
 * issued. Ghost-XR may still have to handle session logic then, but Blender specific handling
 * should be stopped then.
 * This check should be used from external calls to WM_xr. Internally, GHOST_XrSessionIsRunning()
 * may have to be called instead. It checks for the running state according to the OpenXR
 * specification.
 */
bool WM_xr_session_is_running(const wmXrData *xr)
{
  /* wmXrData.session_state will be NULL if session end was requested. That's what we use here to
   * define if the session was already stopped (even if according to OpenXR, it's still considered
   * running). */
  return xr->context && xr->session_state && GHOST_XrSessionIsRunning(xr->context);
}

/** \} */ /* XR-Session */

/* -------------------------------------------------------------------- */
/** \name XR-Session Surface
 *
 * A wmSurface is used to manage drawing of the VR viewport. It's created and destroyed with the
 * session.
 *
 * \{ */

/**
 * \brief Call Ghost-XR to draw a frame
 *
 * Draw callback for the XR-session surface. It's expected to be called on each main loop iteration
 * and tells Ghost-XR to submit a new frame by drawing its views. Note that for drawing each view,
 * #wm_xr_draw_view() will be called through Ghost-XR (see GHOST_XrDrawViewFunc()).
 */
static void wm_xr_session_surface_draw(bContext *C)
{
  wmXrSurfaceData *surface_data = g_xr_surface->customdata;
  wmWindowManager *wm = CTX_wm_manager(C);

  if (!GHOST_XrSessionIsRunning(wm->xr.context)) {
    return;
  }
  GHOST_XrSessionDrawViews(wm->xr.context, C);

  GPU_offscreen_unbind(surface_data->offscreen, false);
}

static void wm_xr_session_free_data(wmSurface *surface)
{
  wmXrSurfaceData *data = surface->customdata;

  if (data->secondary_ghost_ctx) {
#ifdef WIN32
    if (data->gpu_binding_type == GHOST_kXrGraphicsD3D11) {
      WM_directx_context_dispose(data->secondary_ghost_ctx);
    }
#endif
  }
  if (data->viewport) {
    GPU_viewport_free(data->viewport);
  }
  if (data->offscreen) {
    GPU_offscreen_free(data->offscreen);
  }

  MEM_freeN(surface->customdata);

  g_xr_surface = NULL;
}

static bool wm_xr_session_surface_offscreen_ensure(const GHOST_XrDrawViewInfo *draw_view)
{
  wmXrSurfaceData *surface_data = g_xr_surface->customdata;
  const bool size_changed = surface_data->offscreen &&
                            (GPU_offscreen_width(surface_data->offscreen) != draw_view->width) &&
                            (GPU_offscreen_height(surface_data->offscreen) != draw_view->height);
  char err_out[256] = "unknown";
  bool failure = false;

  if (surface_data->offscreen) {
    BLI_assert(surface_data->viewport);

    if (!size_changed) {
      return true;
    }
    GPU_viewport_free(surface_data->viewport);
    GPU_offscreen_free(surface_data->offscreen);
  }

  if (!(surface_data->offscreen = GPU_offscreen_create(
            draw_view->width, draw_view->height, 0, true, false, err_out))) {
    failure = true;
  }

  if (failure) {
    /* Pass. */
  }
  else if (!(surface_data->viewport = GPU_viewport_create())) {
    GPU_offscreen_free(surface_data->offscreen);
    failure = true;
  }

  if (failure) {
    fprintf(stderr, "%s: failed to get buffer, %s\n", __func__, err_out);
    return false;
  }

  return true;
}

wmSurface *wm_xr_session_surface_create(wmWindowManager *UNUSED(wm), unsigned int gpu_binding_type)
{
  if (g_xr_surface) {
    BLI_assert(false);
    return g_xr_surface;
  }

  wmSurface *surface = MEM_callocN(sizeof(*surface), __func__);
  wmXrSurfaceData *data = MEM_callocN(sizeof(*data), "XrSurfaceData");

#ifndef WIN32
  BLI_assert(gpu_binding_type == GHOST_kXrGraphicsOpenGL);
#endif

  surface->draw = wm_xr_session_surface_draw;
  surface->free_data = wm_xr_session_free_data;

  data->gpu_binding_type = gpu_binding_type;
  surface->customdata = data;

  surface->ghost_ctx = DRW_opengl_context_get();

  switch (gpu_binding_type) {
    case GHOST_kXrGraphicsOpenGL:
      break;
#ifdef WIN32
    case GHOST_kXrGraphicsD3D11:
      data->secondary_ghost_ctx = WM_directx_context_create();
      break;
#endif
  }

  surface->gpu_ctx = DRW_gpu_context_get();

  g_xr_surface = surface;

  return surface;
}

/** \} */ /* XR-Session Surface */

/* -------------------------------------------------------------------- */
/** \name XR Drawing
 *
 * \{ */

void wm_xr_pose_to_viewmat(const GHOST_XrPose *pose, float r_viewmat[4][4])
{
  float iquat[4];
  invert_qt_qt_normalized(iquat, pose->orientation_quat);
  quat_to_mat4(r_viewmat, iquat);
  translate_m4(r_viewmat, -pose->position[0], -pose->position[1], -pose->position[2]);
}

/**
 * Proper reference space set up is not supported yet. We simply hand OpenXR the global space as
 * reference space and apply its pose onto the active camera matrix to get a basic viewing
 * experience going. If there's no active camera with stick to the world origin.
 */
static void wm_xr_draw_matrices_create(const GHOST_XrDrawViewInfo *draw_view,
                                       const bXrSessionSettings *session_settings,
                                       const bXrRuntimeSessionState *session_state,
                                       float r_view_mat[4][4],
                                       float r_proj_mat[4][4])
{
  GHOST_XrPose eye_pose;

  copy_qt_qt(eye_pose.orientation_quat, draw_view->eye_pose.orientation_quat);
  copy_v3_v3(eye_pose.position, draw_view->eye_pose.position);
  add_v3_v3(eye_pose.position, session_state->eye_position_ofs);
  if ((session_settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) {
    sub_v3_v3(eye_pose.position, draw_view->local_pose.position);
  }

  perspective_m4_fov(r_proj_mat,
                     draw_view->fov.angle_left,
                     draw_view->fov.angle_right,
                     draw_view->fov.angle_up,
                     draw_view->fov.angle_down,
                     session_settings->clip_start,
                     session_settings->clip_end);

  float eye_mat[4][4];
  float base_mat[4][4];

  wm_xr_pose_to_viewmat(&eye_pose, eye_mat);
  /* Calculate the reference pose matrix (in world space!). */
  wm_xr_pose_to_viewmat(&session_state->final_reference_pose, base_mat);

  mul_m4_m4m4(r_view_mat, eye_mat, base_mat);
}

static void wm_xr_draw_viewport_buffers_to_active_framebuffer(
    const wmXrSurfaceData *surface_data, const GHOST_XrDrawViewInfo *draw_view)
{
  const bool is_upside_down = surface_data->secondary_ghost_ctx &&
                              GHOST_isUpsideDownContext(surface_data->secondary_ghost_ctx);
  rcti rect = {.xmin = 0, .ymin = 0, .xmax = draw_view->width - 1, .ymax = draw_view->height - 1};

  wmViewport(&rect);

  /* For upside down contexts, draw with inverted y-values. */
  if (is_upside_down) {
    SWAP(int, rect.ymin, rect.ymax);
  }
  GPU_viewport_draw_to_screen_ex(surface_data->viewport, &rect, draw_view->expects_srgb_buffer);
}

/**
 * \brief Draw a viewport for a single eye.
 *
 * This is the main viewport drawing function for VR sessions. It's assigned to Ghost-XR as a
 * callback (see GHOST_XrDrawViewFunc()) and executed for each view (read: eye).
 */
void wm_xr_draw_view(const GHOST_XrDrawViewInfo *draw_view, void *customdata)
{
  bContext *C = customdata;
  wmWindowManager *wm = CTX_wm_manager(C);
  wmXrSurfaceData *surface_data = g_xr_surface->customdata;
  bXrSessionSettings *settings = &wm->xr.session_settings;
  Scene *scene = CTX_data_scene(C);

  const float display_flags = V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS | settings->draw_flags;

  float viewmat[4][4], winmat[4][4];

  /* The runtime may still trigger drawing while a session-end request is pending. */
  if (!wm->xr.session_state || !wm->xr.context) {
    return;
  }

  wm_xr_runtime_session_state_final_reference_pose_update(
      wm->xr.session_state, draw_view, settings, scene);
  wm_xr_draw_matrices_create(draw_view, settings, wm->xr.session_state, viewmat, winmat);

  wm_xr_runtime_session_state_info_update(wm->xr.session_state, draw_view, settings);

  if (!wm_xr_session_surface_offscreen_ensure(draw_view)) {
    return;
  }

  /* In case a framebuffer is still bound from drawing the last eye. */
  GPU_framebuffer_restore();

  /* Draws the view into the surface_data->viewport's framebuffers */
  ED_view3d_draw_offscreen_simple(CTX_data_ensure_evaluated_depsgraph(C),
                                  scene,
                                  &wm->xr.session_settings.shading,
                                  wm->xr.session_settings.shading.type,
                                  draw_view->width,
                                  draw_view->height,
                                  display_flags,
                                  viewmat,
                                  winmat,
                                  settings->clip_start,
                                  settings->clip_end,
                                  true,
                                  true,
                                  NULL,
                                  false,
                                  surface_data->offscreen,
                                  surface_data->viewport);

  /* Re-use the offscreen framebuffer to render the composited viewport into. Keep it bound,
   * Ghost-XR will then blit from the currently bound framebuffer into the OpenXR swapchain. */
  GPU_offscreen_bind(surface_data->offscreen, false);

  wm_xr_draw_viewport_buffers_to_active_framebuffer(surface_data, draw_view);
}

/** \} */ /* XR Drawing */