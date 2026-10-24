// Video: what the GPU decodes for programs (D3D11 video decoding, DXVA: players, browsers, ffmpeg's d3d11va), whether
// a frame decoded on it is right (a key frame per codec, compared with the software decoder's picture), the video
// processor (color conversion and scaling, how Media Foundation and browsers show decoded frames) and shader views of
// NV12's planes (how players like mpv show them). Without decoding programs decode on the CPU.

#include "video_samples.inc"

struct VideoSample {
   const unsigned char *picture, *qmatrix, *stream;
   UINT pictureSize, qmatrixSize, streamSize;
   uint32_t hash;
   bool annexb;   // H.264/HEVC: slices after start codes; VP9: one frame
   const unsigned char *tiles = nullptr;   // AV1: the slice control as it is (DXVA_Tile_AV1 entries)
   UINT tilesSize = 0;
};

struct VideoProfile {
   GUID guid;
   const char *name;
   DXGI_FORMAT format;
   const VideoSample *sample;
};

static const VideoSample SAMPLE_H264 = {SAMPLE_H264_PICTURE, SAMPLE_H264_QMATRIX, SAMPLE_H264_STREAM,
                                        sizeof SAMPLE_H264_PICTURE, sizeof SAMPLE_H264_QMATRIX, sizeof SAMPLE_H264_STREAM,
                                        SAMPLE_H264_HASH, true};
static const VideoSample SAMPLE_HEVC = {SAMPLE_HEVC_PICTURE, nullptr, SAMPLE_HEVC_STREAM, sizeof SAMPLE_HEVC_PICTURE, 0,
                                        sizeof SAMPLE_HEVC_STREAM, SAMPLE_HEVC_HASH, true};
static const VideoSample SAMPLE_VP9 = {SAMPLE_VP9_PICTURE, nullptr, SAMPLE_VP9_STREAM, sizeof SAMPLE_VP9_PICTURE, 0,
                                       sizeof SAMPLE_VP9_STREAM, SAMPLE_VP9_HASH, false};
static const VideoSample SAMPLE_AV1 = {SAMPLE_AV1_PICTURE, nullptr, SAMPLE_AV1_STREAM, sizeof SAMPLE_AV1_PICTURE, 0,
                                       sizeof SAMPLE_AV1_STREAM, SAMPLE_AV1_HASH, false, SAMPLE_AV1_TILES, sizeof SAMPLE_AV1_TILES};

static const VideoProfile VIDEO_PROFILES[] = {
   {{0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}}, "H.264 (AVC) decoding", DXGI_FORMAT_NV12, &SAMPLE_H264},
   {{0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}}, "H.265 (HEVC) Main decoding", DXGI_FORMAT_NV12, &SAMPLE_HEVC},
   {{0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}}, "H.265 (HEVC) Main 10 decoding", DXGI_FORMAT_P010, nullptr},
   {{0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}}, "VP9 decoding", DXGI_FORMAT_NV12, &SAMPLE_VP9},
   {{0xa4c749ef, 0x6ecf, 0x48aa, {0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7}}, "VP9 10-bit (profile 2) decoding", DXGI_FORMAT_P010, nullptr},
   {{0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}}, "AV1 decoding", DXGI_FORMAT_NV12, &SAMPLE_AV1},
   {{0xee27417f, 0x5e28, 0x4e65, {0xbe, 0xea, 0x1d, 0x26, 0xb5, 0x08, 0xad, 0xc9}}, "MPEG-2 decoding", DXGI_FORMAT_NV12, nullptr},
};

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
   for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
   return h;
}

// An NV12 picture as the software decoder makes it: the luma rows, then the chroma rows
static bool nv12Hash(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, UINT w, UINT h, uint32_t &hash)
{
   D3D11_TEXTURE2D_DESC sd = {w, h, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
   ID3D11Texture2D *staging = nullptr;
   if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return false;
   ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
   D3D11_MAPPED_SUBRESOURCE m;
   bool ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
   if (ok) {
      hash = 0x811c9dc5u;
      const uint8_t *p = (const uint8_t *)m.pData;
      for (UINT y = 0; y < h + h / 2; y++) hash = fnv1a(hash, p + y * m.RowPitch, w);
      ctx->Unmap(staging, 0);
   }
   release(staging);
   return ok;
}

// A sample's slice control: DXVA_Slice_*_Short entries (location, size, wBadSliceChopping 0) for H.264/HEVC at their
// start codes and VP9's whole frame, AV1's tiles as they are; count is the number of entries
static std::vector<uint8_t> sliceControl(const VideoSample &s, UINT &count)
{
   std::vector<uint32_t> starts;
   for (UINT i = 0; s.annexb && i + 3 <= s.streamSize; i++) {
      if (!s.stream[i] && !s.stream[i + 1] && s.stream[i + 2] == 1) starts.push_back(i);
   }
   if (starts.empty()) starts.push_back(0);
   count = (UINT)starts.size();
   std::vector<uint8_t> slices(s.tiles, s.tiles + s.tilesSize);
   for (size_t i = 0; !s.tiles && i < starts.size(); i++) {
      uint32_t end = i + 1 < starts.size() ? starts[i + 1] : s.streamSize;
      uint32_t entry[2] = {starts[i], end - starts[i]};
      uint8_t raw[10] = {};
      memcpy(raw, entry, 8);
      slices.insert(slices.end(), raw, raw + 10);
   }
   return slices;
}

// One key frame through the decoder: the picture parameters, quantization matrix, slice control and bitstream as
// ffmpeg's d3d11va submits them. Shared: into a texture shared with an NT handle, read on another device (as browsers
// hand decoded frames to their compositor's device)
static void decodeSample(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *video, ID3D11VideoContext *vctx,
                         const VideoProfile &p, bool shared = false)
{
   const VideoSample &s = *p.sample;
   const std::string name = shared ? std::string(p.name) + " into a shared texture" : p.name;
   const UINT W = SAMPLE_SIZE, H = SAMPLE_SIZE;
   std::string detail;
   ID3D11Texture2D *tex = nullptr;
   ID3D11VideoDecoder *decoder = nullptr;
   ID3D11VideoDecoderOutputView *view = nullptr;
   uint32_t hash = 0;
   bool ok = false;

   D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, p.format, {1, 0}, D3D11_USAGE_DEFAULT,
                              (UINT)(shared ? D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_DECODER), 0,
                              (UINT)(shared ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0)};
   ID3D11Device *other = nullptr;
   ID3D11Device1 *other1 = nullptr;
   ID3D11DeviceContext *otherCtx = nullptr;
   ID3D11Texture2D *opened = nullptr;
   IDXGIResource1 *res = nullptr;
   HANDLE handle = nullptr;
   D3D11_VIDEO_DECODER_DESC dd = {p.guid, W, H, p.format};
   D3D11_VIDEO_DECODER_CONFIG cfg = {};
   UINT configs = 0;
   HRESULT h = dev->CreateTexture2D(&td, nullptr, &tex);
   if (SUCCEEDED(h)) h = video->GetVideoDecoderConfigCount(&dd, &configs);
   for (UINT i = 0; SUCCEEDED(h) && i < configs; i++) {
      // the short slice format (H.264: 2; the others have only it)
      if (SUCCEEDED(video->GetVideoDecoderConfig(&dd, i, &cfg)) && (!s.qmatrix || cfg.ConfigBitstreamRaw == 2)) break;
   }
   if (SUCCEEDED(h)) h = video->CreateVideoDecoder(&dd, &cfg, &decoder);
   D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {p.guid, D3D11_VDOV_DIMENSION_TEXTURE2D};
   if (SUCCEEDED(h)) h = video->CreateVideoDecoderOutputView(tex, &vd, &view);
   if (SUCCEEDED(h)) h = vctx->DecoderBeginFrame(decoder, view, 0, nullptr);
   if (FAILED(h)) {
      report(Status::Fail, name, "setting up the decoder failed (" + hrs(h) + ")");
      goto out;
   }
   {
      std::vector<D3D11_VIDEO_DECODER_BUFFER_DESC> descs;
      UINT sliceCount = 0;
      std::vector<uint8_t> slices = sliceControl(s, sliceCount);
      struct Part { D3D11_VIDEO_DECODER_BUFFER_TYPE type; const void *data; UINT size; } parts[] = {
         {D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, s.picture, s.pictureSize},
         {D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, s.qmatrix, s.qmatrixSize},
         {D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, slices.data(), (UINT)slices.size()},
         {D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, s.stream, s.streamSize},
      };
      for (auto &part : parts) {
         if (!part.size) continue;
         void *ptr = nullptr;
         UINT size = 0;
         h = vctx->GetDecoderBuffer(decoder, part.type, &size, &ptr);
         if (FAILED(h) || size < part.size) {
            detail = FAILED(h) ? "GetDecoderBuffer failed (" + hrs(h) + ")" : "decoder buffer too small";
            break;
         }
         memcpy(ptr, part.data, part.size);
         vctx->ReleaseDecoderBuffer(decoder, part.type);
         D3D11_VIDEO_DECODER_BUFFER_DESC d = {};
         d.BufferType = part.type;
         d.DataSize = part.size;
         if (part.type == D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL) d.NumMBsInBuffer = sliceCount;
         descs.push_back(d);
      }
      if (detail.empty()) {
         h = vctx->SubmitDecoderBuffers(decoder, (UINT)descs.size(), descs.data());
         if (FAILED(h)) detail = "SubmitDecoderBuffers failed (" + hrs(h) + ")";
      }
      vctx->DecoderEndFrame(decoder);
   }
   if (detail.empty() && shared) {
      ctx->Flush();
      const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
      createDevice11(&other, &otherCtx, &fl, 1);
      h = other ? other->QueryInterface(__uuidof(ID3D11Device1), (void **)&other1) : E_FAIL;
      if (SUCCEEDED(h)) h = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res);
      if (SUCCEEDED(h)) h = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
      if (SUCCEEDED(h)) h = other1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&opened);
      if (FAILED(h)) detail = "sharing it with another device failed (" + hrs(h) + ")";
   }
   if (detail.empty()) {
      if (!nv12Hash(shared ? other : dev, shared ? otherCtx : ctx, shared ? opened : tex, W, H, hash)) detail = "reading the picture back failed";
      else if (hash == s.hash) ok = true, detail = shared ? "a key frame decoded on the GPU, read on another device: identical to the software decoder's"
                                                          : "a key frame decoded on the GPU, identical to the software decoder's";
      else detail = fmt("the decoded picture differs from the software decoder's (hash %08x, expected %08x)", hash, s.hash);
   }
   check(ok, name, detail);
out:
   if (handle) CloseHandle(handle);
   release(res);
   release(opened);
   release(otherCtx);
   release(other1);
   release(other);
   release(view);
   release(decoder);
   release(tex);
}

// The video processor: NV12 (pure red, BT.601 studio range) to BGRA, scaled 64x64 to 128x96. Or the same red in a
// packed 4:4:4 format (HEVC 4:4:4's decoder output): AYUV (V, U, Y, A bytes) or Y410 (U, Y, V, A in 10:10:10:2)
static void processorTest(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *video, ID3D11VideoContext *vctx,
                          DXGI_FORMAT format = DXGI_FORMAT_NV12)
{
   const char *name = format == DXGI_FORMAT_AYUV ? "Video processor (AYUV input)"
                    : format == DXGI_FORMAT_Y410 ? "Video processor (Y410 input)"
                                                 : "Video processor (color conversion and scaling)";
   if (!startTest(name)) return;
   const UINT W = 64, H = 64, OW = 128, OH = 96;
   std::vector<uint8_t> nv12(W * H * 3 / 2, 81);   // Y 81, Cb 90, Cr 240: red
   for (UINT i = W * H; i < nv12.size(); i += 2) nv12[i] = 90, nv12[i + 1] = 240;
   std::vector<uint32_t> packed(W * H, format == DXGI_FORMAT_AYUV ? 0xff515af0u : 360u | 324u << 10 | 960u << 20 | 3u << 30);
   D3D11_SUBRESOURCE_DATA init = {nv12.data(), W, 0};
   if (format != DXGI_FORMAT_NV12) init = {packed.data(), W * 4, 0};
   // (the runtime takes NV12 textures bound for decoding, as render targets or not at all as video processor input, not
   // those bound only as shader resources; the packed ones bound for decoding)
   D3D11_TEXTURE2D_DESC in = {W, H, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
                              format == DXGI_FORMAT_NV12 ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_DECODER, 0, 0};
   D3D11_TEXTURE2D_DESC outd = {OW, OH, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
   ID3D11Texture2D *src = nullptr, *dst = nullptr;
   ID3D11VideoProcessorEnumerator *vpe = nullptr;
   ID3D11VideoProcessor *vp = nullptr;
   ID3D11VideoProcessorInputView *iv = nullptr;
   ID3D11VideoProcessorOutputView *ov = nullptr;
   D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {30, 1}, W, H, {30, 1}, OW, OH,
                                            D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
   const char *step = "the input texture";
   HRESULT h = dev->CreateTexture2D(&in, &init, &src);
   if (SUCCEEDED(h)) step = "the BGRA texture", h = dev->CreateTexture2D(&outd, nullptr, &dst);
   if (SUCCEEDED(h)) step = "CreateVideoProcessorEnumerator", h = video->CreateVideoProcessorEnumerator(&cd, &vpe);
   if (SUCCEEDED(h)) step = "CreateVideoProcessor", h = video->CreateVideoProcessor(vpe, 0, &vp);
   D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0, D3D11_VPIV_DIMENSION_TEXTURE2D};
   D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {D3D11_VPOV_DIMENSION_TEXTURE2D};
   if (SUCCEEDED(h)) step = "CreateVideoProcessorInputView", h = video->CreateVideoProcessorInputView(src, vpe, &ivd, &iv);
   if (SUCCEEDED(h)) step = "CreateVideoProcessorOutputView", h = video->CreateVideoProcessorOutputView(dst, vpe, &ovd, &ov);
   // the packed formats are needed where the device decodes into them (HEVC 4:4:4), optional elsewhere
   UINT support = 0;
   bool required = format == DXGI_FORMAT_NV12 ||
                   (SUCCEEDED(dev->CheckFormatSupport(format, &support)) && (support & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT));
   if (FAILED(h)) {
      std::string why = fmt("not supported: %s failed (%s)", step, hrs(h).c_str());
      if (required) missing(name, why);
      else optional(false, name, why + " (no decoding into it here: optional)");
   } else {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};   // BT.601, studio range in; full range RGB out
      cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
      vctx->VideoProcessorSetStreamColorSpace(vp, 0, &cs);
      // the picture in the middle, green around it
      RECT dstRect = {OW / 4, OH / 4, OW * 3 / 4, OH * 3 / 4};
      D3D11_VIDEO_COLOR green = {};
      green.RGBA.G = 1, green.RGBA.A = 1;
      vctx->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dstRect);
      vctx->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &green);
      D3D11_VIDEO_PROCESSOR_STREAM stream = {TRUE, 0, 0, 0, 0, nullptr, iv};
      h = vctx->VideoProcessorBlt(vp, ov, 0, 1, &stream);
      D3D11_TEXTURE2D_DESC sd = outd;
      sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Texture2D *staging = nullptr;
      uint32_t px[2] = {};
      if (SUCCEEDED(h)) h = dev->CreateTexture2D(&sd, nullptr, &staging);
      if (SUCCEEDED(h)) {
         ctx->CopyResource(staging, dst);
         D3D11_MAPPED_SUBRESOURCE m;
         h = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
         if (SUCCEEDED(h)) {
            px[0] = ((const uint32_t *)m.pData)[0];
            px[1] = ((const uint32_t *)((const uint8_t *)m.pData + (OH / 2) * m.RowPitch))[OW / 2];
            ctx->Unmap(staging, 0);
         }
      }
      release(staging);
      auto is = [](uint32_t c, int r, int g, int b) {
         return near8((c >> 16) & 0xff, r, 12) && near8((c >> 8) & 0xff, g, 12) && near8(c & 0xff, b, 12);
      };
      check(SUCCEEDED(h) && is(px[0], 0, 255, 0) && is(px[1], 255, 0, 0), name,
            FAILED(h) ? "VideoProcessorBlt failed (" + hrs(h) + ")"
                      : fmt("%s 64x64 (red) scaled into the middle of a BGRA 128x96, green around it: corner %06x, middle "
                            "%06x, expected 00ff00, ff0000", format == DXGI_FORMAT_NV12 ? "NV12" : format == DXGI_FORMAT_AYUV ? "AYUV" : "Y410", px[0] & 0xffffff, px[1] & 0xffffff));
   }
   release(ov);
   release(iv);
   release(vp);
   release(vpe);
   release(dst);
   release(src);
}

// Bob deinterlacing in the video processor: an interlaced NV12 frame whose top field is light (Y 200) and bottom field
// dark (Y 50); output 0 (the first field, top) must come out evenly light and output 1 evenly dark (without
// deinterlacing the rows alternate). Motion-adaptive (adaptive): with the same frame before and after it nothing
// moves, so both outputs keep the alternating rows (woven); with an even grey frame (Y 120) before and after it
// they come out like bob
static void deinterlaceTest(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11VideoDevice *video,
                            ID3D11VideoContext *vctx, bool adaptive)
{
   const char *name = adaptive ? "Video processor (motion-adaptive deinterlacing)" : "Video processor (bob deinterlacing)";
   if (!startTest(name)) return;
   const UINT W = 64, H = 64;
   std::vector<uint8_t> nv12(W * H * 3 / 2, 128);
   for (UINT y = 0; y < H; y++) memset(&nv12[y * W], y & 1 ? 50 : 200, W);
   std::vector<uint8_t> grey(W * H * 3 / 2, 128);
   memset(grey.data(), 120, W * H);
   D3D11_SUBRESOURCE_DATA init = {nv12.data(), W, 0}, initGrey = {grey.data(), W, 0};
   D3D11_TEXTURE2D_DESC in = {W, H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
   D3D11_TEXTURE2D_DESC outd = {W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
   ID3D11Texture2D *src = nullptr, *dst = nullptr, *staging = nullptr, *same = nullptr, *moved = nullptr;
   ID3D11VideoProcessorEnumerator *vpe = nullptr;
   ID3D11VideoProcessor *vp = nullptr;
   ID3D11VideoProcessorInputView *iv = nullptr, *ivSame = nullptr, *ivMoved = nullptr;
   ID3D11VideoProcessorOutputView *ov = nullptr;
   D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST, {30, 1}, W, H, {60, 1}, W, H,
                                            D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
   D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rc = {};
   const char *step = "the NV12 texture";
   HRESULT h = dev->CreateTexture2D(&in, &init, &src);
   if (SUCCEEDED(h)) step = "the BGRA texture", h = dev->CreateTexture2D(&outd, nullptr, &dst);
   D3D11_TEXTURE2D_DESC sd = outd;
   sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   if (SUCCEEDED(h)) step = "the staging texture", h = dev->CreateTexture2D(&sd, nullptr, &staging);
   if (SUCCEEDED(h)) step = "CreateVideoProcessorEnumerator", h = video->CreateVideoProcessorEnumerator(&cd, &vpe);
   if (SUCCEEDED(h)) step = "GetVideoProcessorRateConversionCaps", h = vpe->GetVideoProcessorRateConversionCaps(0, &rc);
   if (SUCCEEDED(h) && !(rc.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB))
      step = "the rate conversion caps (no bob)", h = E_NOTIMPL;
   if (SUCCEEDED(h) && adaptive &&
       (!(rc.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE) || rc.PastFrames < 1 ||
        rc.FutureFrames < 1))
      step = "the rate conversion caps (no adaptive with a past and a future frame)", h = E_NOTIMPL;
   if (SUCCEEDED(h)) step = "CreateVideoProcessor", h = video->CreateVideoProcessor(vpe, 0, &vp);
   D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0, D3D11_VPIV_DIMENSION_TEXTURE2D};
   D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {D3D11_VPOV_DIMENSION_TEXTURE2D};
   if (SUCCEEDED(h)) step = "CreateVideoProcessorInputView", h = video->CreateVideoProcessorInputView(src, vpe, &ivd, &iv);
   if (SUCCEEDED(h)) step = "CreateVideoProcessorOutputView", h = video->CreateVideoProcessorOutputView(dst, vpe, &ovd, &ov);
   if (SUCCEEDED(h) && adaptive) step = "the past and future NV12 textures", h = dev->CreateTexture2D(&in, &init, &same);
   if (SUCCEEDED(h) && adaptive) h = dev->CreateTexture2D(&in, &initGrey, &moved);
   if (SUCCEEDED(h) && adaptive) step = "their input views", h = video->CreateVideoProcessorInputView(same, vpe, &ivd, &ivSame);
   if (SUCCEEDED(h) && adaptive) h = video->CreateVideoProcessorInputView(moved, vpe, &ivd, &ivMoved);
   if (FAILED(h)) {
      missing(name, fmt("not supported: %s failed (%s)", step, hrs(h).c_str()));
   } else {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
      cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
      vctx->VideoProcessorSetStreamColorSpace(vp, 0, &cs);
      vctx->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
      // per frame around it (adaptive: the same, then grey) and output: the green of rows H/2 and H/2 + 1, the
      // middle column
      int g[2][2][2] = {};
      for (UINT k = 0; k < (adaptive ? 4u : 2u) && SUCCEEDED(h); k++) {
         UINT o = k & 1;
         ID3D11VideoProcessorInputView *around = k < 2 ? ivSame : ivMoved;
         D3D11_VIDEO_PROCESSOR_STREAM stream = {TRUE, o, 0, adaptive, adaptive, adaptive ? &around : nullptr, iv,
                                                adaptive ? &around : nullptr};
         h = vctx->VideoProcessorBlt(vp, ov, o, 1, &stream);
         if (FAILED(h)) break;
         ctx->CopyResource(staging, dst);
         D3D11_MAPPED_SUBRESOURCE m;
         h = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
         if (FAILED(h)) break;
         for (UINT r = 0; r < 2; r++)
            g[k / 2][o][r] = (((const uint32_t *)((const uint8_t *)m.pData + (H / 2 + r) * m.RowPitch))[W / 2] >> 8) & 0xff;
         ctx->Unmap(staging, 0);
      }
      // Y 200 and 50 in studio range: 214 and 40 in RGB
      const int(*bob)[2] = g[adaptive];
      bool ok = near8(bob[0][0], 214, 12) && near8(bob[0][1], 214, 12) && near8(bob[1][0], 40, 12) &&
                near8(bob[1][1], 40, 12);
      std::string got = fmt("output 0 rows %d %d (expected 214), output 1 rows %d %d (expected 40)", bob[0][0],
                            bob[0][1], bob[1][0], bob[1][1]);
      if (adaptive) {
         ok = ok && near8(g[0][0][0], 214, 12) && near8(g[0][0][1], 40, 12) && near8(g[0][1][0], 214, 12) &&
              near8(g[0][1][1], 40, 12);
         got = fmt("the same frames around it: output 0 rows %d %d, output 1 rows %d %d (expected 214 40); grey "
                   "frames around it: ", g[0][0][0], g[0][0][1], g[0][1][0], g[0][1][1]) + got;
      }
      check(SUCCEEDED(h) && ok, name,
            FAILED(h) ? "VideoProcessorBlt failed (" + hrs(h) + ")" : "top field light, bottom dark: " + got);
   }
   release(ov);
   release(ivMoved);
   release(ivSame);
   release(iv);
   release(vp);
   release(vpe);
   release(moved);
   release(same);
   release(staging);
   release(dst);
   release(src);
}

// Shader views of NV12's planes (R8: luma, R8G8: chroma), read by a compute shader
static void planeViewTest(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
   const char *name = "Shader views of NV12 planes (luma R8, chroma R8G8)";
   if (!startTest(name)) return;
   const UINT W = 64, H = 64;
   std::vector<uint8_t> nv12(W * H * 3 / 2, 100);
   for (UINT i = W * H; i < nv12.size(); i += 2) nv12[i] = 50, nv12[i + 1] = 200;
   D3D11_SUBRESOURCE_DATA init = {nv12.data(), W, 0};
   D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
   ID3D11Texture2D *tex = nullptr;
   ID3D11ShaderResourceView *luma = nullptr, *chroma = nullptr;
   ID3D11Buffer *buf = nullptr, *staging = nullptr;
   ID3D11UnorderedAccessView *uav = nullptr;
   ID3D11ComputeShader *cs = nullptr;
   ID3DBlob *blob = nullptr;
   static const char *hlsl = "Texture2D<float> y : register(t0); Texture2D<float2> uv : register(t1);\n"
                             "RWBuffer<float> o : register(u0);\n"
                             "[numthreads(1, 1, 1)] void main() { o[0] = y.Load(int3(5, 5, 0)); float2 c = uv.Load(int3(3, 3, 0));"
                             " o[1] = c.x; o[2] = c.y; }\n";
   D3D11_SHADER_RESOURCE_VIEW_DESC sv = {DXGI_FORMAT_R8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D};
   sv.Texture2D.MipLevels = 1;
   HRESULT h = dev->CreateTexture2D(&td, &init, &tex);
   if (SUCCEEDED(h)) h = dev->CreateShaderResourceView(tex, &sv, &luma);
   sv.Format = DXGI_FORMAT_R8G8_UNORM;
   if (SUCCEEDED(h)) h = dev->CreateShaderResourceView(tex, &sv, &chroma);
   if (FAILED(h)) {
      missing(name, "not supported (" + hrs(h) + ")");
   } else {
      D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {DXGI_FORMAT_R32_FLOAT, D3D11_UAV_DIMENSION_BUFFER};
      ud.Buffer.NumElements = 4;
      h = compileD3D(hlsl, strlen(hlsl), nullptr, "main", "cs_5_0", 0, &blob, nullptr);
      if (SUCCEEDED(h)) h = dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
      if (SUCCEEDED(h)) h = dev->CreateBuffer(&bd, nullptr, &buf);
      if (SUCCEEDED(h)) h = dev->CreateUnorderedAccessView(buf, &ud, &uav);
      bd.Usage = D3D11_USAGE_STAGING, bd.BindFlags = 0, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (SUCCEEDED(h)) h = dev->CreateBuffer(&bd, nullptr, &staging);
      float v[4] = {-1, -1, -1, -1};
      if (SUCCEEDED(h)) {
         ID3D11ShaderResourceView *views[2] = {luma, chroma};
         ctx->CSSetShader(cs, nullptr, 0);
         ctx->CSSetShaderResources(0, 2, views);
         ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
         ctx->Dispatch(1, 1, 1);
         ctx->CopyResource(staging, buf);
         D3D11_MAPPED_SUBRESOURCE m;
         h = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
         if (SUCCEEDED(h)) {
            memcpy(v, m.pData, sizeof v);
            ctx->Unmap(staging, 0);
         }
         ID3D11ShaderResourceView *none[2] = {};
         ID3D11UnorderedAccessView *noUav = nullptr;
         ctx->CSSetShaderResources(0, 2, none);
         ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
      }
      bool ok = SUCCEEDED(h) && near8((int)lroundf(v[0] * 255), 100, 1) && near8((int)lroundf(v[1] * 255), 50, 1) &&
                near8((int)lroundf(v[2] * 255), 200, 1);
      // the data read back through a copy too: wrong views or wrong data
      uint32_t got = 0, want = fnv1a(0x811c9dc5u, nv12.data(), nv12.size());
      bool data = nv12Hash(dev, ctx, tex, W, H, got) && got == want;
      check(ok, name, FAILED(h) ? hrs(h) : fmt("read Y %.0f Cb %.0f Cr %.0f, expected 100 50 200 (the texture's data %s)", v[0] * 255,
                                               v[1] * 255, v[2] * 255, data ? "is right" : "is wrong too"));
   }
   release(blob);
   release(cs);
   release(staging);
   release(uav);
   release(buf);
   release(chroma);
   release(luma);
   release(tex);
}

static void groupVideo()
{
   if (g_warp) {
      report(Status::Skip, "Video decoding", "no video on WARP");
      return;
   }
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx);
   ID3D11VideoDevice *video = nullptr;
   ID3D11VideoContext *vctx = nullptr;
   if (SUCCEEDED(h)) h = dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&video);
   if (SUCCEEDED(h)) h = ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&vctx);
   if (FAILED(h)) {
      // the runtime refuses D3D11_CREATE_DEVICE_VIDEO_SUPPORT when the driver has no video functions
      report(Status::Warn, "Direct3D 11 video device", "not supported by the GPU's driver (" + hrs(h) + "): programs decode on the CPU");
      for (auto &p : VIDEO_PROFILES) report(Status::Skip, p.name, "no video device");
      release(vctx);
      release(video);
      release(ctx);
      release(dev);
      return;
   }
   check(true, "Direct3D 11 video device");

   UINT count = video->GetVideoDecoderProfileCount();
   std::vector<GUID> profiles(count);
   for (UINT i = 0; i < count; i++) video->GetVideoDecoderProfile(i, &profiles[i]);
   report(Status::Info, "Decoder profiles", fmt("%u", count));
   for (auto &p : VIDEO_PROFILES) {
      if (!startTest(p.name)) continue;
      bool has = std::find(profiles.begin(), profiles.end(), p.guid) != profiles.end();
      BOOL format = FALSE;
      if (has) video->CheckVideoDecoderFormat(&p.guid, p.format, &format);
      if (!has || !format) report(Status::Warn, p.name, has ? "the decoder has no output format" : "not offered by the GPU's driver: decoded on the CPU");
      else if (p.sample) decodeSample(dev, ctx, video, vctx, p);
      else report(Status::Pass, p.name, "on the GPU");
   }
   // (H.264's)
   const char *sharedName = "H.264 (AVC) decoding into a shared texture";
   if (startTest(sharedName)) {
      D3D11_FEATURE_DATA_D3D11_OPTIONS4 o4 = {};
      bool nv12 = SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS4, &o4, sizeof o4)) &&
                  o4.ExtendedNV12SharedTextureSupported;
      bool h264 = std::find(profiles.begin(), profiles.end(), VIDEO_PROFILES[0].guid) != profiles.end();
      if (!nv12 || !h264) optional(false, sharedName, !nv12 ? "the driver doesn't offer shared NV12 textures (11.4)" : "no H.264 decoding");
      else decodeSample(dev, ctx, video, vctx, VIDEO_PROFILES[0], true);
   }

   // (no NV12 textures: Windows 7's Direct3D 11 takes them only from drivers with the 11.1 video DDI, which it has none
   // of - and reports the format supported all the same, so one is made to see)
   D3D11_TEXTURE2D_DESC probe = {64, 64, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
   ID3D11Texture2D *nv12Probe = nullptr;
   bool nv12 = SUCCEEDED(dev->CreateTexture2D(&probe, nullptr, &nv12Probe));
   release(nv12Probe);
   if (!nv12) {
      for (const char *n : {"Video processor (color conversion and scaling)", "Video processor (AYUV input)",
                            "Video processor (Y410 input)", "Video processor (bob deinterlacing)",
                            "Video processor (motion-adaptive deinterlacing)", "Shader views of NV12 planes (luma R8, chroma R8G8)"})
         if (startTest(n)) optional(false, n, "no NV12 textures on this Direct3D 11 (Windows 7: no video driver interface)");
      release(vctx);
      release(video);
      release(ctx);
      release(dev);
      return;
   }
   processorTest(dev, ctx, video, vctx);
   processorTest(dev, ctx, video, vctx, DXGI_FORMAT_AYUV);
   processorTest(dev, ctx, video, vctx, DXGI_FORMAT_Y410);
   deinterlaceTest(dev, ctx, video, vctx, false);
   deinterlaceTest(dev, ctx, video, vctx, true);
   planeViewTest(dev, ctx);
   release(vctx);
   release(video);
   release(ctx);
   release(dev);
}
