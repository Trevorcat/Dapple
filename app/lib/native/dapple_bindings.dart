// ignore_for_file: library_private_types_in_public_api
// Hand-written dart:ffi bindings for core/include/dapple/dapple_c.h (FROZEN contract).
import 'dart:ffi' as ffi;
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

/// `DappleSession` is opaque.
final class DappleSessionStruct extends ffi.Opaque {}

/// `DappleImage`: RGBA8888, stride == width * 4, owned by the library.
final class DappleImageStruct extends ffi.Struct {
  @ffi.Int32()
  external int width;
  @ffi.Int32()
  external int height;
  external ffi.Pointer<ffi.Uint8> rgba;
}

/// `DappleFrameRequest` (dapple_c.h): one interactive frame / zoom region.
final class DappleFrameRequestStruct extends ffi.Struct {
  @ffi.Int32()
  external int size;
  @ffi.Int32()
  external int longEdge;
  @ffi.Int32()
  external int flags;
  @ffi.Int32()
  external int bakeSize;
  @ffi.Float()
  external double rx0;
  @ffi.Float()
  external double ry0;
  @ffi.Float()
  external double rx1;
  @ffi.Float()
  external double ry1;
  @ffi.Int32()
  external int regionLongEdge;
  /// Additive (honoured when [size] covers it): key of the slider being dragged; grain stays on in interactive frames while a
  /// grain-family slider moves (pipeline-architecture.md 14, Q8). nullptr = none.
  external ffi.Pointer<Utf8> activeParam;
}

const int dappleFrameInteractive = 1;
const int dappleFrameAnalysis = 2;
const int dappleFrameExact = 4;
const int dappleFrameBackground = 8;

const int dappleOk = 0;
const int dappleError = 1;
const int dappleQualityPreview = 0;
const int dappleQualityFull = 1;

typedef _CStrFn = ffi.Pointer<Utf8> Function();
typedef _CStrFnD = ffi.Pointer<Utf8> Function();
typedef _CStrArg = ffi.Pointer<Utf8> Function(ffi.Pointer<Utf8>);
typedef _PerfConf1 = ffi.Void Function(ffi.Int32);
typedef _PerfConf1D = void Function(int);
typedef _Info0 = ffi.Pointer<Utf8> Function();
typedef _Info0D = ffi.Pointer<Utf8> Function();
typedef _TouchHint = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Int32);
typedef _TouchHintD = int Function(ffi.Pointer<DappleSessionStruct>, int);
typedef _PerfConf = ffi.Void Function(ffi.Int32, ffi.Int32);
typedef _PerfConfD = void Function(int, int);
typedef _ThermalFn = ffi.Float Function(ffi.Int32);
typedef _ThermalFnD = double Function(int);
typedef _FreeStr = ffi.Void Function(ffi.Pointer<Utf8>);
typedef _FreeStrD = void Function(ffi.Pointer<Utf8>);
typedef _FreeImg = ffi.Void Function(ffi.Pointer<DappleImageStruct>);
typedef _FreeImgD = void Function(ffi.Pointer<DappleImageStruct>);
typedef _IsSupported = ffi.Int32 Function(ffi.Pointer<Utf8>);
typedef _IsSupportedD = int Function(ffi.Pointer<Utf8>);
typedef _Open = ffi.Pointer<DappleSessionStruct> Function(ffi.Pointer<Utf8>, ffi.Int32, ffi.Int32);
typedef _OpenD = ffi.Pointer<DappleSessionStruct> Function(ffi.Pointer<Utf8>, int, int);
typedef _OpenRegion = ffi.Pointer<DappleSessionStruct> Function(ffi.Pointer<Utf8>, ffi.Int32, ffi.Int32, ffi.Int32, ffi.Int32);
typedef _OpenRegionD = ffi.Pointer<DappleSessionStruct> Function(ffi.Pointer<Utf8>, int, int, int, int);
typedef _VoidFn = ffi.Void Function();
typedef _VoidFnD = void Function();
typedef _Close = ffi.Void Function(ffi.Pointer<DappleSessionStruct>);
typedef _CloseD = void Function(ffi.Pointer<DappleSessionStruct>);
typedef _Cancel = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>);
typedef _CancelD = int Function(ffi.Pointer<DappleSessionStruct>);
typedef _Info = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>);
typedef _Auto = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Float);
typedef _AutoD = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, double);
typedef _Render = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>,
    ffi.Int32, ffi.Pointer<DappleImageStruct>);
typedef _RenderD = int Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, int, ffi.Pointer<DappleImageStruct>);
typedef _Export = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>,
    ffi.Int32, ffi.Int32, ffi.Pointer<Utf8>);
typedef _ExportD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, int, int,
    ffi.Pointer<Utf8>);

typedef _ExportEx = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>,
    ffi.Int32, ffi.Int32, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _ExportExD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, int, int,
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _ExportHdrEx = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Int32,
    ffi.Int32, ffi.Pointer<Utf8>, ffi.Float, ffi.Pointer<ffi.Uint8>, ffi.Pointer<Utf8>);
typedef _ExportHdrExD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, int, int,
    ffi.Pointer<Utf8>, double, ffi.Pointer<ffi.Uint8>, ffi.Pointer<Utf8>);
typedef _ExportHdr = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Int32,
    ffi.Int32, ffi.Pointer<Utf8>, ffi.Float, ffi.Pointer<ffi.Uint8>);
typedef _ExportHdrD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, int, int,
    ffi.Pointer<Utf8>, double, ffi.Pointer<ffi.Uint8>);
typedef _HdrProbe = ffi.Int32 Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Uint8>);
typedef _HdrProbeD = int Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Uint8>);
typedef _WideProbe = ffi.Int32 Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Float>);
typedef _WideProbeD = int Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Float>);

typedef _RenderFrame = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>,
    ffi.Pointer<DappleFrameRequestStruct>, ffi.Pointer<DappleImageStruct>, ffi.Pointer<ffi.Pointer<Utf8>>);
typedef _RenderFrameD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>,
    ffi.Pointer<DappleFrameRequestStruct>, ffi.Pointer<DappleImageStruct>, ffi.Pointer<ffi.Pointer<Utf8>>);

typedef _AnalyzeP = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>);
typedef _AnalyzePD = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>);

typedef _Previews = ffi.Int32 Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Int32, ffi.Pointer<DappleImageStruct>);
typedef _PreviewsD = int Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, int, ffi.Pointer<DappleImageStruct>);
// dapple_session_base_previews(session, params_json, const char* const* base_ids, n, long_edge, DappleImage* out)
typedef _BasePreviews = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Pointer<Utf8>>, ffi.Int32,
    ffi.Int32, ffi.Pointer<DappleImageStruct>);
typedef _BasePreviewsD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Pointer<Utf8>>, int, int,
    ffi.Pointer<DappleImageStruct>);
typedef _BasePreviewsCancel = ffi.Void Function(ffi.Pointer<DappleSessionStruct>);
typedef _BasePreviewsCancelD = void Function(ffi.Pointer<DappleSessionStruct>);
typedef _TonePreviews = ffi.Int32 Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Int32, ffi.Float, ffi.Pointer<DappleImageStruct>);
typedef _TonePreviewsD = int Function(
    ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, int, double, ffi.Pointer<DappleImageStruct>);

typedef _EmbedThumb = ffi.Int32 Function(ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _EmbedThumbD = int Function(ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _ExtractPreview = ffi.Int32 Function(ffi.Pointer<Utf8>, ffi.Int32, ffi.Pointer<DappleImageStruct>);
typedef _ExtractPreviewD = int Function(ffi.Pointer<Utf8>, int, ffi.Pointer<DappleImageStruct>);

/// `DappleProgressCell` (dapple_c.h): latest progress written by `dapple_progress_to_cell`.
final class DappleProgressCell extends ffi.Struct {
  @ffi.Float()
  external double fraction;
  @ffi.Int32()
  external int stage;
}

typedef DappleProgressNative = ffi.Int32 Function(ffi.Float, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Void>);
typedef _MergeAvail = ffi.Int32 Function();
typedef _MergeAvailD = int Function();
typedef _OpenMerged = ffi.Pointer<DappleSessionStruct> Function(
    ffi.Pointer<ffi.Pointer<Utf8>>,
    ffi.Int32,
    ffi.Pointer<Utf8>,
    ffi.Pointer<Utf8>,
    ffi.Pointer<ffi.NativeFunction<DappleProgressNative>>,
    ffi.Pointer<ffi.Void>,
    ffi.Pointer<ffi.Int32>,
    ffi.Pointer<ffi.Pointer<Utf8>>);
typedef _OpenMergedD = ffi.Pointer<DappleSessionStruct> Function(
    ffi.Pointer<ffi.Pointer<Utf8>>,
    int,
    ffi.Pointer<Utf8>,
    ffi.Pointer<Utf8>,
    ffi.Pointer<ffi.NativeFunction<DappleProgressNative>>,
    ffi.Pointer<ffi.Void>,
    ffi.Pointer<ffi.Int32>,
    ffi.Pointer<ffi.Pointer<Utf8>>);
typedef _SaveLinear = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _SaveLinearD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _Detect = ffi.Pointer<Utf8> Function(
    ffi.Pointer<ffi.Pointer<Utf8>>,
    ffi.Int32,
    ffi.Pointer<Utf8>,
    ffi.Pointer<ffi.NativeFunction<DappleProgressNative>>,
    ffi.Pointer<ffi.Void>,
    ffi.Pointer<ffi.Int32>);
typedef _DetectD = ffi.Pointer<Utf8> Function(
    ffi.Pointer<ffi.Pointer<Utf8>>,
    int,
    ffi.Pointer<Utf8>,
    ffi.Pointer<ffi.NativeFunction<DappleProgressNative>>,
    ffi.Pointer<ffi.Void>,
    ffi.Pointer<ffi.Int32>);
typedef _MotionWrite = ffi.Int32 Function(
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Double, ffi.Pointer<ffi.Pointer<Utf8>>);
typedef _MotionWriteD = int Function(
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, double, ffi.Pointer<ffi.Pointer<Utf8>>);
typedef _MotionFromVideo = ffi.Int32 Function(
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Pointer<Utf8>>);
typedef _MotionFromVideoD = int Function(
    ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<ffi.Pointer<Utf8>>);

typedef _LoadMem = ffi.Int32 Function(ffi.Pointer<ffi.Void>, ffi.Size);
typedef _LoadMemD = int Function(ffi.Pointer<ffi.Void>, int);

class DappleBindings {
  DappleBindings(ffi.DynamicLibrary lib)
      : version = lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_version'),
        lastError = lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_last_error'),
        freeString = lib.lookupFunction<_FreeStr, _FreeStrD>('dapple_free_string'),
        freeImage = lib.lookupFunction<_FreeImg, _FreeImgD>('dapple_free_image'),
        probeJson = lib.lookupFunction<_CStrArg, _CStrArg>('dapple_probe_json'),
        xmpToJson = lib.lookupFunction<_CStrArg, _CStrArg>('dapple_xmp_to_json'),
        isSupportedFile = lib.lookupFunction<_IsSupported, _IsSupportedD>('dapple_is_supported_file'),
        sessionOpen = lib.lookupFunction<_Open, _OpenD>('dapple_session_open'),
        sessionClose = lib.lookupFunction<_Close, _CloseD>('dapple_session_close'),
        sessionExportJpegHdr = lib.providesSymbol('dapple_session_export_jpeg_hdr')
            ? lib.lookupFunction<_ExportHdr, _ExportHdrD>('dapple_session_export_jpeg_hdr')
            : null,
        sessionExportJpegEx = lib.providesSymbol('dapple_session_export_jpeg_ex')
            ? lib.lookupFunction<_ExportEx, _ExportExD>('dapple_session_export_jpeg_ex')
            : null,
        sessionExportJpegHdrEx = lib.providesSymbol('dapple_session_export_jpeg_hdr_ex')
            ? lib.lookupFunction<_ExportHdrEx, _ExportHdrExD>('dapple_session_export_jpeg_hdr_ex')
            : null,
        sessionHdrHeadroom = lib.providesSymbol('dapple_session_hdr_headroom')
            ? lib.lookupFunction<_HdrProbe, _HdrProbeD>('dapple_session_hdr_headroom')
            : null,
        sessionWideGamutFraction = lib.providesSymbol('dapple_session_wide_gamut_fraction')
            ? lib.lookupFunction<_WideProbe, _WideProbeD>('dapple_session_wide_gamut_fraction')
            : null,
        sessionCancel = lib.providesSymbol('dapple_session_cancel') ? lib.lookupFunction<_Cancel, _CancelD>('dapple_session_cancel') : null,
        sessionInfoJson = lib.lookupFunction<_Info, _Info>('dapple_session_info_json'),
        sessionAutoJson = lib.lookupFunction<_Auto, _AutoD>('dapple_session_auto_json'),
        sessionRender = lib.lookupFunction<_Render, _RenderD>('dapple_session_render'),
        sessionExportJpeg = lib.lookupFunction<_Export, _ExportD>('dapple_session_export_jpeg'),
        // Interactive frames (image + analysis in one call, baked lattice, regions); null on older libraries.
        sessionRenderFrame = lib.providesSymbol('dapple_session_render_frame')
            ? lib.lookupFunction<_RenderFrame, _RenderFrameD>('dapple_session_render_frame')
            : null,
        // Added by the engine work running in parallel: guard against older libraries.
        perfConfigure = lib.providesSymbol('dapple_perf_configure')
            ? lib.lookupFunction<_PerfConf, _PerfConfD>('dapple_perf_configure')
            : null,
        touchHint = lib.providesSymbol('dapple_session_touch_hint') ? lib.lookupFunction<_TouchHint, _TouchHintD>('dapple_session_touch_hint') : null,
        setInteraction = lib.providesSymbol('dapple_set_interaction') ? lib.lookupFunction<_PerfConf1, _PerfConf1D>('dapple_set_interaction') : null,
        threadBackground = lib.providesSymbol('dapple_thread_background')
            ? lib.lookupFunction<_PerfConf1, _PerfConf1D>('dapple_thread_background')
            : null,
        thermalHeadroom = lib.providesSymbol('dapple_thermal_headroom')
            ? lib.lookupFunction<_ThermalFn, _ThermalFnD>('dapple_thermal_headroom')
            : null,
        perfInfoJson = lib.providesSymbol('dapple_perf_info_json')
            ? lib.lookupFunction<_Info0, _Info0D>('dapple_perf_info_json')
            : null,
        sessionAnalyzeJson = lib.providesSymbol('dapple_session_analyze_json')
            ? lib.lookupFunction<_Info, _Info>('dapple_session_analyze_json')
            : null,
        sessionFilmPadJson = lib.providesSymbol('dapple_session_film_pad_json')
            ? lib.lookupFunction<_AnalyzeP, _AnalyzePD>('dapple_session_film_pad_json')
            : null,
        sessionBasePreviews = lib.providesSymbol('dapple_session_base_previews')
            ? lib.lookupFunction<_BasePreviews, _BasePreviewsD>('dapple_session_base_previews')
            : null,
        sessionBasePreviewsCancel = lib.providesSymbol('dapple_session_base_previews_cancel')
            ? lib.lookupFunction<_BasePreviewsCancel, _BasePreviewsCancelD>('dapple_session_base_previews_cancel')
            : null,
        sessionAnalyzeParamsJson = lib.providesSymbol('dapple_session_analyze_params_json')
            ? lib.lookupFunction<_AnalyzeP, _AnalyzePD>('dapple_session_analyze_params_json')
            : null,
        looksJson = lib.providesSymbol('dapple_looks_json') ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_looks_json') : null,
        paramsSchemaJson = lib.providesSymbol('dapple_params_schema_json')
            ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_params_schema_json')
            : null,
        sessionLookPreviews = lib.providesSymbol('dapple_session_render_look_previews')
            ? lib.lookupFunction<_Previews, _PreviewsD>('dapple_session_render_look_previews')
            : null,
        sessionTonePreviews = lib.providesSymbol('dapple_session_render_tone_previews')
            ? lib.lookupFunction<_TonePreviews, _TonePreviewsD>('dapple_session_render_tone_previews')
            : null,
        dngEmbedThumbnail = lib.providesSymbol('dapple_dng_embed_thumbnail')
            ? lib.lookupFunction<_EmbedThumb, _EmbedThumbD>('dapple_dng_embed_thumbnail')
            : null,
        extractPreview = lib.providesSymbol('dapple_extract_preview')
            ? lib.lookupFunction<_ExtractPreview, _ExtractPreviewD>('dapple_extract_preview')
            : null,
        mergeAvailable = lib.providesSymbol('dapple_merge_available')
            ? lib.lookupFunction<_MergeAvail, _MergeAvailD>('dapple_merge_available')
            : null,
        sessionOpenRegion = lib.providesSymbol('dapple_session_open_region')
            ? lib.lookupFunction<_OpenRegion, _OpenRegionD>('dapple_session_open_region')
            : null,
        regionCancel = lib.providesSymbol('dapple_region_cancel') ? lib.lookupFunction<_VoidFn, _VoidFnD>('dapple_region_cancel') : null,
        sessionOpenMerged = lib.providesSymbol('dapple_session_open_merged')
            ? lib.lookupFunction<_OpenMerged, _OpenMergedD>('dapple_session_open_merged')
            : null,
        sessionSaveLinear = lib.providesSymbol('dapple_session_save_linear')
            ? lib.lookupFunction<_SaveLinear, _SaveLinearD>('dapple_session_save_linear')
            : null,
        detectSequences = lib.providesSymbol('dapple_detect_sequences_json')
            ? lib.lookupFunction<_Detect, _DetectD>('dapple_detect_sequences_json')
            : null,
        videoProbeJson = lib.providesSymbol('dapple_video_probe_json')
            ? lib.lookupFunction<_CStrArg, _CStrArg>('dapple_video_probe_json')
            : null,
        motionPhotoWrite = lib.providesSymbol('dapple_motion_photo_write')
            ? lib.lookupFunction<_MotionWrite, _MotionWriteD>('dapple_motion_photo_write')
            : null,
        motionPhotoFromVideo = lib.providesSymbol('dapple_motion_photo_from_video')
            ? lib.lookupFunction<_MotionFromVideo, _MotionFromVideoD>('dapple_motion_photo_from_video')
            : null,
        motionPhotoValidate = lib.providesSymbol('dapple_motion_photo_validate_json')
            ? lib.lookupFunction<_CStrArg, _CStrArg>('dapple_motion_photo_validate_json')
            : null,
        filmLoadPackMemory = lib.providesSymbol('dapple_film_load_pack_memory')
            ? lib.lookupFunction<_LoadMem, _LoadMemD>('dapple_film_load_pack_memory')
            : null,
        filmListJson = lib.providesSymbol('dapple_film_list_json') ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_film_list_json') : null,
        filmPrebakeStart = lib.providesSymbol('dapple_film_prebake_start') ? lib.lookupFunction<_VoidFn, _VoidFnD>('dapple_film_prebake_start') : null,
        filmPrebakeStatus = lib.providesSymbol('dapple_film_prebake_status') ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_film_prebake_status') : null,
        filmAttribution =
            lib.providesSymbol('dapple_film_attribution') ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_film_attribution') : null,
        filmSwatchesJson =
            lib.providesSymbol('dapple_film_swatches_json') ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_film_swatches_json') : null,
        filmSwatchBandsJson = lib.providesSymbol('dapple_film_swatch_bands_json')
            ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_film_swatch_bands_json')
            : null,
        gpuStatusJson =
            lib.providesSymbol('dapple_gpu_status_json') ? lib.lookupFunction<_CStrFn, _CStrFnD>('dapple_gpu_status_json') : null,
        progressToCell = lib.providesSymbol('dapple_progress_to_cell')
            ? lib.lookup<ffi.NativeFunction<DappleProgressNative>>('dapple_progress_to_cell')
            : null;

  final _CStrFnD version;
  final _CStrFnD lastError;
  final _FreeStrD freeString;
  final _FreeImgD freeImage;
  final _CStrArg probeJson;
  final _CStrArg xmpToJson;
  final _IsSupportedD isSupportedFile;
  final _OpenD sessionOpen;
  final _CloseD sessionClose;
  final _CancelD? sessionCancel; // lock-free, safe from any thread while a render is in flight
  final _Info sessionInfoJson;
  final _AutoD sessionAutoJson;
  final _RenderD sessionRender;
  final _ExportD sessionExportJpeg;
  final _ExportHdrD? sessionExportJpegHdr; // Ultra HDR (gain-map JPEG); null when the library predates it
  final _ExportExD? sessionExportJpegEx; // export with Exif / ICC (metadata options JSON); null when the library predates it
  final _ExportHdrExD? sessionExportJpegHdrEx;
  final _HdrProbeD? sessionHdrHeadroom;
  final _WideProbeD? sessionWideGamutFraction;
  final _RenderFrameD? sessionRenderFrame;
  final _PerfConfD? perfConfigure;
  final _PerfConf1D? threadBackground;
  final _PerfConf1D? setInteraction; // dapple_set_interaction: background demosaic tiles wait while a finger is on a control
  final _TouchHintD? touchHint; // dapple_session_touch_hint: keep-warm burst on pointer-down (wave 8)
  final _ThermalFnD? thermalHeadroom;
  final _Info0D? perfInfoJson;
  final _Info? sessionAnalyzeJson;
  final _AnalyzePD? sessionAnalyzeParamsJson;
  final _BasePreviewsD? sessionBasePreviews; // batch swatches of rendering bases (one small interactive frame per base id)
  final _BasePreviewsCancelD? sessionBasePreviewsCancel; // lock-free, any thread

  /// dapple_session_film_pad_json(session, params_json): the film pads' labels and effective ranges (optional symbol).
  final _AnalyzePD? sessionFilmPadJson;
  final _CStrFnD? looksJson;
  final _CStrFnD? paramsSchemaJson;
  final _PreviewsD? sessionLookPreviews;
  final _TonePreviewsD? sessionTonePreviews;
  final _ExtractPreviewD? extractPreview;
  final _EmbedThumbD? dngEmbedThumbnail;
  final _MergeAvailD? mergeAvailable;
  final _OpenMergedD? sessionOpenMerged;
  final _OpenRegionD? sessionOpenRegion;
  final _VoidFnD? regionCancel;
  final _SaveLinearD? sessionSaveLinear;
  final _DetectD? detectSequences;
  final _CStrArg? videoProbeJson;
  final _MotionWriteD? motionPhotoWrite;
  final _MotionFromVideoD? motionPhotoFromVideo;
  final _CStrArg? motionPhotoValidate;
  final _LoadMemD? filmLoadPackMemory;
  final _CStrFnD? filmListJson;
  final _VoidFnD? filmPrebakeStart; // dapple_film_prebake_start
  final _CStrFnD? filmPrebakeStatus;
  final _CStrFnD? filmAttribution;
  final _CStrFnD? filmSwatchesJson;
  final _CStrFnD? filmSwatchBandsJson;
  final _CStrFnD? gpuStatusJson; // GPU probe state, device, self-test, per-frame executor counters
  final ffi.Pointer<ffi.NativeFunction<DappleProgressNative>>? progressToCell;

  /// Opens the native library for the current platform. Throws if missing.
  static ffi.DynamicLibrary openLibrary() {
    // Desktop tests / tools: DAPPLE_LIB points at a host build of the engine (build/<dir>/bin/libdapple.dll, libdapple.so ...).
    final override = Platform.environment['DAPPLE_LIB'];
    if (override != null && override.isNotEmpty && !Platform.isAndroid && !Platform.isIOS) return ffi.DynamicLibrary.open(override);
    if (Platform.isAndroid) return ffi.DynamicLibrary.open('libdapple.so');
    if (Platform.isIOS || Platform.isMacOS) {
      // iOS: the engine is dapple.framework (xcframework in the dapple_engine plugin, embedded and signed by Xcode). Its symbols are
      // visible to process() when dyld loaded the framework at launch; otherwise open it by name.
      final self = ffi.DynamicLibrary.process();
      if (self.providesSymbol('dapple_version')) return self;
      if (Platform.isIOS) return ffi.DynamicLibrary.open('dapple.framework/dapple');
      return self;
    }
    if (Platform.isWindows) return ffi.DynamicLibrary.open('dapple.dll');
    return ffi.DynamicLibrary.open('libdapple.so');
  }

  static DappleBindings? _instance;
  static Object? loadError;

  /// Lazily loaded singleton; null when the native library is unavailable.
  static DappleBindings? get instanceOrNull {
    if (_instance != null) return _instance;
    try {
      _instance = DappleBindings(openLibrary());
    } catch (e) {
      loadError = e;
    }
    return _instance;
  }
}
