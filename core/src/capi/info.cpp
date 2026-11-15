#include "session.h"

namespace {

const char* supportName(Support s) {
  switch (s) {
    case Support::Yes: return "yes";
    case Support::No: return "no";
    default: return "unverified";
  }
}

std::string metaJson(const ShotMetadata& m, const CameraProfile& cam) {
  std::string j = "{";
  j += "\"make\":" + q(m.make) + ",\"model\":" + q(m.model) + ",\"lens\":" + q(m.lens);
  j += ",\"iso\":" + n(m.iso) + ",\"shutter\":" + n(m.shutterSeconds) + ",\"aperture\":" + n(m.aperture);
  j += ",\"focalLength\":" + n(m.focalLengthMm) + ",\"timestamp\":" + std::to_string(m.timestamp);
  j += ",\"width\":" + std::to_string(m.sensorWidth) + ",\"height\":" + std::to_string(m.sensorHeight);
  j += ",\"cfa\":" + q(toString(m.cfa == CfaKind::Unknown ? cam.cfa : m.cfa)) + ",\"rawFormat\":" + q(m.rawFormat);
  j += ",\"inCameraLook\":" + q(m.inCameraLook);
  j += ",\"camera\":{\"id\":" + q(cam.id) + ",\"displayName\":" + q(cam.displayName);
  j += ",\"ftp\":" + q(supportName(cam.transfer.ftpPush));
  j += ",\"usbPtp\":" + q(supportName(cam.transfer.usbPtp));
  j += ",\"vendorAppAutoSend\":" + q(supportName(cam.transfer.vendorAppAutoSend));
  j += ",\"vendorApp\":" + q(cam.transfer.vendorApp) + "}}";
  return j;
}

}  // namespace

int32_t dapple_dng_embed_thumbnail(const char* dng_path, const char* jpeg_path) {
  return guarded(
      [&]() -> int32_t {
        if (!dng_path || !jpeg_path) {
          setError("null path");
          return DAPPLE_ERROR;
        }
        std::string err;
        if (!embedDngThumbnailFile(toPath(dng_path), toPath(jpeg_path), &err)) {
          setError(err);
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_extract_preview(const char* path, int32_t long_edge, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!out) {
          setError("null output");
          return DAPPLE_ERROR;
        }
        *out = DappleImage{0, 0, nullptr};
        auto r = extractEmbeddedPreview(toPath(path), long_edge);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        return fillImage(r.value(), out) ? DAPPLE_OK : DAPPLE_ERROR;
      },
      DAPPLE_ERROR);
}

char* dapple_probe_json(const char* path) {
  return guarded(
      [&]() -> char* {
        const fs::path p = toPath(path);
        const ImageDecoder* decoder = decoders().find(p);
        if (!decoder) {
          setError("Unsupported file type: " + pathToUtf8(p));
          return nullptr;
        }
        auto meta = decoder->probe(p);
        if (!meta) {
          setError(meta.error().message);
          return nullptr;
        }
        return dupString(metaJson(*meta, cameras().resolve(meta->make, meta->model)));
      },
      nullptr);
}

char* dapple_xmp_to_json(const char* xmp_path) {
  return guarded(
      [&]() -> char* {
        auto preset = loadXmpPreset(toPath(xmp_path));
        if (!preset) {
          setError(preset.error().message);
          return nullptr;
        }
        std::string j = "{\"name\":" + q(preset->name) + ",\"params\":" + toJson(preset->params) + ",\"unsupported\":[";
        for (std::size_t i = 0; i < preset->unsupported.size(); ++i) j += (i ? "," : "") + q(preset->unsupported[i]);
        j += "]}";
        return dupString(j);
      },
      nullptr);
}

int32_t dapple_is_supported_file(const char* path) {
  return guarded([&]() -> int32_t { return decoders().find(toPath(path)) != nullptr ? 1 : 0; }, 0);
}

char* dapple_session_info_json(DappleSession* session) {
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        return dupString(metaJson(session->source.meta, *session->camera));
      },
      nullptr);
}

char* dapple_session_auto_json(DappleSession* session, float strength) {
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        if (!ensureSource(session)) return nullptr;
        AutoOptions opt;
        opt.strength = strength;
        const AutoResult r = computeAutoAdjust(session->source, *session->camera, opt);
        const ImageStats& st = r.stats;
        std::string j = "{\"correction\":" + toJson(r.correction) + ",\"stats\":{";
        j += "\"medianLuma\":" + n(st.medianLuma) + ",\"p01Luma\":" + n(st.p01Luma) + ",\"p99Luma\":" + n(st.p99Luma);
        j += ",\"clippedFraction\":" + n(st.clippedFraction) + ",\"neutralCastA\":" + n(st.neutralCastA);
        j += ",\"neutralCastB\":" + n(st.neutralCastB) + ",\"neutralCoverage\":" + n(st.neutralCoverage);
        j += "},\"notes\":[";
        for (std::size_t i = 0; i < r.notes.size(); ++i) j += (i ? "," : "") + q(r.notes[i]);
        j += "]}";
        return dupString(j);
      },
      nullptr);
}
