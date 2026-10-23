// Direct2D on the GPU's Direct3D 11 device: each scene drawn on it and on WARP, the pixels compared

static const UINT D2W = 64, D2H = 64;

struct D2D {
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   ID2D1Factory1 *factory = nullptr;
   ID2D1Device *d2dev = nullptr;
   ID2D1DeviceContext *dc = nullptr;
   IDWriteFactory *dwrite = nullptr;
   ID3D11Texture2D *tex = nullptr;   // the target, B8G8R8A8 (GDI compatible for the interop scene)
   ID2D1Bitmap1 *target = nullptr;
   void release()
   {
      ::release(target);
      ::release(tex);
      ::release(dc);
      ::release(d2dev);
      ::release(dwrite);
      ::release(factory);
      ::release(ctx);
      ::release(dev);
   }
};

static bool setupD2D(D2D &d, bool warp, std::string &detail)
{
   const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                       D3D_FEATURE_LEVEL_10_0};
   HRESULT h = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION, &d.dev, nullptr, &d.ctx);
   if (h == E_INVALIDARG) {  // (Windows 7's runtime refuses a list with 11_1 in it)
      h = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3, D3D11_SDK_VERSION, &d.dev, nullptr, &d.ctx);
   }
   if (FAILED(h)) {
      detail = "D3D11CreateDevice (BGRA support): " + hrs(h);
      return false;
   }
   D2D1_FACTORY_OPTIONS fo = {};
   h = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void **)&d.factory);
   if (FAILED(h)) {
      detail = "D2D1CreateFactory (Direct2D 1.1): " + hrs(h);
      return false;
   }
   IDXGIDevice *dxgi = nullptr;
   d.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
   h = dxgi ? d.factory->CreateDevice(dxgi, &d.d2dev) : E_NOINTERFACE;
   release(dxgi);
   if (FAILED(h)) {
      detail = "ID2D1Factory1::CreateDevice: " + hrs(h);
      return false;
   }
   h = d.d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d.dc);
   if (FAILED(h)) {
      detail = "CreateDeviceContext: " + hrs(h);
      return false;
   }
   DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&d.dwrite);

   D3D11_TEXTURE2D_DESC td = {D2W, D2H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_GDI_COMPATIBLE};
   h = d.dev->CreateTexture2D(&td, nullptr, &d.tex);
   if (FAILED(h)) {
      detail = "CreateTexture2D (GDI compatible B8G8R8A8): " + hrs(h);
      return false;
   }
   IDXGISurface *surface = nullptr;
   d.tex->QueryInterface(__uuidof(IDXGISurface), (void **)&surface);
   D2D1_BITMAP_PROPERTIES1 bp = {};
   bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
   bp.dpiX = bp.dpiY = 96;
   bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
   h = surface ? d.dc->CreateBitmapFromDxgiSurface(surface, &bp, &d.target) : E_NOINTERFACE;
   release(surface);
   if (FAILED(h)) {
      detail = "CreateBitmapFromDxgiSurface: " + hrs(h);
      return false;
   }
   d.dc->SetTarget(d.target);
   d.dc->SetDpi(96, 96);
   return true;
}

static bool readD2D(D2D &d, std::vector<uint32_t> &out)
{
   D3D11_TEXTURE2D_DESC sd = {D2W, D2H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                              D3D11_CPU_ACCESS_READ, 0};
   ID3D11Texture2D *st = nullptr;
   if (FAILED(d.dev->CreateTexture2D(&sd, nullptr, &st))) return false;
   d.ctx->CopyResource(st, d.tex);
   D3D11_MAPPED_SUBRESOURCE m;
   bool ok = SUCCEEDED(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
   if (ok) {
      out.resize(D2W * D2H);
      for (UINT y = 0; y < D2H; y++) memcpy(&out[y * D2W], (char *)m.pData + y * m.RowPitch, D2W * 4);
      d.ctx->Unmap(st, 0);
   }
   st->Release();
   return ok;
}

static ID2D1SolidColorBrush *solid(D2D &d, float r, float g, float b, float a = 1)
{
   ID2D1SolidColorBrush *brush = nullptr;
   d.dc->CreateSolidColorBrush(D2D1_COLOR_F{r, g, b, a}, &brush);
   return brush;
}

static ID2D1Bitmap1 *checkerBitmap(D2D &d, UINT size)
{
   std::vector<uint32_t> px(size * size);
   for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) px[y * size + x] = ((x / 2 + y / 2) & 1) ? 0xff2040c0 : 0xffe0c040;
   D2D1_BITMAP_PROPERTIES1 bp = {};
   bp.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
   bp.dpiX = bp.dpiY = 96;
   ID2D1Bitmap1 *bmp = nullptr;
   d.dc->CreateBitmap(D2D1_SIZE_U{size, size}, px.data(), size * 4, &bp, &bmp);
   return bmp;
}

static ID2D1PathGeometry *wavePath(D2D &d)
{
   ID2D1PathGeometry *path = nullptr;
   ID2D1GeometrySink *sink = nullptr;
   d.factory->CreatePathGeometry(&path);
   if (!path || FAILED(path->Open(&sink))) {
      release(path);
      return nullptr;
   }
   sink->BeginFigure(D2D1_POINT_2F{6, 50}, D2D1_FIGURE_BEGIN_FILLED);
   sink->AddBezier(D2D1_BEZIER_SEGMENT{{14, 2}, {30, 2}, {34, 30}});
   sink->AddQuadraticBezier(D2D1_QUADRATIC_BEZIER_SEGMENT{{44, 60}, {58, 20}});
   sink->AddArc(D2D1_ARC_SEGMENT{{40, 58}, {14, 9}, 30, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
   sink->AddLine(D2D1_POINT_2F{12, 60});
   sink->EndFigure(D2D1_FIGURE_END_CLOSED);
   sink->Close();
   sink->Release();
   return path;
}

typedef std::function<bool(D2D &, std::string &)> D2DScene;

struct D2DTest {
   const char *name;
   D2DScene scene;
   bool optionalFeature;
};

static const D2DTest D2D_TESTS[] = {
   {"Solid rectangles and lines (aliased)", [](D2D &d, std::string &e) {
       ID2D1SolidColorBrush *b = solid(d, 0.9f, 0.2f, 0.1f), *b2 = solid(d, 0.1f, 0.5f, 0.9f, 0.5f);
       d.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
       d.dc->FillRectangle(D2D1_RECT_F{4, 4, 40, 30}, b);
       d.dc->FillRectangle(D2D1_RECT_F{20, 16, 60, 60}, b2);
       d.dc->DrawLine(D2D1_POINT_2F{2, 62}, D2D1_POINT_2F{62, 34}, b, 3);
       release(b);
       release(b2);
       return true;
    }},
   {"Antialiased ellipses and rounded rectangles", [](D2D &d, std::string &e) {
       ID2D1SolidColorBrush *b = solid(d, 0.2f, 0.7f, 0.3f), *b2 = solid(d, 0.8f, 0.1f, 0.6f, 0.75f);
       d.dc->FillEllipse(D2D1_ELLIPSE{{24, 22}, 19, 13}, b);
       d.dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{{14.5f, 28.5f, 58, 58}, 9, 6}, b2, 3.5f);
       d.dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{{40, 6, 61, 26}, 5, 5}, b2);
       release(b);
       release(b2);
       return true;
    }},
   {"Path geometry (Bezier curves, arcs; fill and stroke)", [](D2D &d, std::string &e) {
       ID2D1PathGeometry *path = wavePath(d);
       ID2D1SolidColorBrush *b = solid(d, 0.95f, 0.6f, 0.1f), *b2 = solid(d, 0.1f, 0.1f, 0.4f);
       bool ok = path && b && b2;
       if (ok) {
          d.dc->FillGeometry(path, b);
          d.dc->DrawGeometry(path, b2, 1.5f);
       }
       release(path);
       release(b);
       release(b2);
       return ok;
    }},
   {"Stroke styles (dashes, caps, joins)", [](D2D &d, std::string &e) {
       ID2D1StrokeStyle1 *style = nullptr;
       const float dashes[] = {3, 1.5f, 0.5f, 1.5f};
       D2D1_STROKE_STYLE_PROPERTIES1 sp = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_TRIANGLE, D2D1_CAP_STYLE_SQUARE,
                                           D2D1_LINE_JOIN_MITER_OR_BEVEL, 4, D2D1_DASH_STYLE_CUSTOM, 0.5f,
                                           D2D1_STROKE_TRANSFORM_TYPE_NORMAL};
       d.factory->CreateStrokeStyle(sp, dashes, 4, &style);
       ID2D1PathGeometry *path = wavePath(d);
       ID2D1SolidColorBrush *b = solid(d, 0.1f, 0.3f, 0.8f);
       bool ok = style && path && b;
       if (ok) {
          d.dc->DrawGeometry(path, b, 4, style);
          d.dc->DrawLine(D2D1_POINT_2F{4, 8}, D2D1_POINT_2F{60, 12}, b, 5, style);
       }
       release(style);
       release(path);
       release(b);
       return ok;
    }},
   {"Linear and radial gradient brushes", [](D2D &d, std::string &e) {
       const D2D1_GRADIENT_STOP stops[] = {{0, {1, 0, 0, 1}}, {0.5f, {1, 1, 0, 0.8f}}, {1, {0, 0, 1, 1}}};
       ID2D1GradientStopCollection *coll = nullptr;
       d.dc->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_MIRROR, &coll);
       ID2D1LinearGradientBrush *lin = nullptr;
       ID2D1RadialGradientBrush *rad = nullptr;
       if (coll) {
          d.dc->CreateLinearGradientBrush(D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES{{0, 0}, {40, 20}}, coll, &lin);
          d.dc->CreateRadialGradientBrush(D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES{{44, 44}, {4, -3}, 18, 12}, coll, &rad);
       }
       bool ok = lin && rad;
       if (ok) {
          d.dc->FillRectangle(D2D1_RECT_F{0, 0, 64, 32}, lin);
          d.dc->FillEllipse(D2D1_ELLIPSE{{44, 44}, 19, 19}, rad);
       }
       release(lin);
       release(rad);
       release(coll);
       return ok;
    }},
   {"Bitmaps (scaled, rotated, linear and cubic interpolation, opacity)", [](D2D &d, std::string &e) {
       ID2D1Bitmap1 *bmp = checkerBitmap(d, 8);
       if (!bmp) return false;
       d.dc->DrawBitmap(bmp, D2D1_RECT_F{2, 2, 30, 30}, 1, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
       d.dc->DrawBitmap(bmp, D2D1_RECT_F{34, 2, 62, 30}, 0.7f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                        nullptr);
       D2D1_MATRIX_3X2_F rot = {0.866f, 0.5f, -0.5f, 0.866f, 32, 30};
       d.dc->SetTransform(rot);
       d.dc->DrawBitmap(bmp, D2D1_RECT_F{0, 0, 24, 24}, 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
       d.dc->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
       bmp->Release();
       return true;
    }},
   {"Bitmap brushes (tiled, transformed)", [](D2D &d, std::string &e) {
       ID2D1Bitmap1 *bmp = checkerBitmap(d, 8);
       ID2D1BitmapBrush1 *brush = nullptr;
       if (bmp) {
          D2D1_BITMAP_BRUSH_PROPERTIES1 bp = {D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_MIRROR,
                                              D2D1_INTERPOLATION_MODE_LINEAR};
          d.dc->CreateBitmapBrush(bmp, &bp, nullptr, &brush);
       }
       bool ok = brush != nullptr;
       if (ok) {
          brush->SetTransform(D2D1_MATRIX_3X2_F{1.5f, 0.3f, -0.2f, 1.2f, 3, 1});
          d.dc->FillEllipse(D2D1_ELLIPSE{{32, 32}, 30, 26}, brush);
       }
       release(brush);
       release(bmp);
       return ok;
    }},
   {"Layers (opacity, geometric mask) and axis-aligned clips", [](D2D &d, std::string &e) {
       ID2D1PathGeometry *path = wavePath(d);
       ID2D1SolidColorBrush *b = solid(d, 0.2f, 0.8f, 0.9f), *b2 = solid(d, 0.9f, 0.3f, 0.2f);
       bool ok = path && b && b2;
       if (ok) {
          D2D1_LAYER_PARAMETERS1 lp = {D2D1_RECT_F{0, 0, 64, 64}, path, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                       D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0}, 0.6f, nullptr, D2D1_LAYER_OPTIONS1_NONE};
          d.dc->PushLayer(lp, nullptr);
          d.dc->FillRectangle(D2D1_RECT_F{0, 0, 64, 64}, b);
          d.dc->PopLayer();
          d.dc->PushAxisAlignedClip(D2D1_RECT_F{10.5f, 20.25f, 50, 40}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
          d.dc->FillEllipse(D2D1_ELLIPSE{{30, 30}, 25, 25}, b2);
          d.dc->PopAxisAlignedClip();
       }
       release(path);
       release(b);
       release(b2);
       return ok;
    }},
   {"Opacity masks (FillOpacityMask)", [](D2D &d, std::string &e) {
       ID2D1Bitmap1 *mask = checkerBitmap(d, 16);
       ID2D1SolidColorBrush *b = solid(d, 0.4f, 0.1f, 0.9f);
       bool ok = mask && b;
       if (ok) {
          d.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
          D2D1_RECT_F r = {4, 4, 60, 60}, src = {0, 0, 16, 16};
          d.dc->FillOpacityMask(mask, b, &r, &src);
       }
       release(mask);
       release(b);
       return ok;
    }},
   {"Text (DirectWrite, grayscale antialiasing)", [](D2D &d, std::string &e) {
       if (!d.dwrite) {
          e = "no DirectWrite";
          return false;
       }
       IDWriteTextFormat *format = nullptr;
       d.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 17, L"en-us", &format);
       ID2D1SolidColorBrush *b = solid(d, 0.05f, 0.05f, 0.1f);
       bool ok = format && b;
       if (ok) {
          d.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
          d.dc->DrawText(L"WinBoat Ag", 10, format, D2D1_RECT_F{2, 2, 64, 64}, b);
       }
       release(format);
       release(b);
       return ok;
    }},
   {"Effects: Gaussian blur", [](D2D &d, std::string &e) {
       ID2D1Bitmap1 *bmp = checkerBitmap(d, 32);
       ID2D1Effect *blur = nullptr;
       d.dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
       bool ok = bmp && blur;
       if (ok) {
          blur->SetInput(0, bmp);
          blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 2.0f);
          D2D1_POINT_2F at = {16, 16};
          d.dc->DrawImage(blur, &at);
       }
       release(blur);
       release(bmp);
       return ok;
    }},
   {"Effects: color matrix, shadow and composite", [](D2D &d, std::string &e) {
       ID2D1Bitmap1 *bmp = checkerBitmap(d, 24);
       ID2D1Effect *matrix = nullptr, *shadow = nullptr, *comp = nullptr;
       d.dc->CreateEffect(CLSID_D2D1ColorMatrix, &matrix);
       d.dc->CreateEffect(CLSID_D2D1Shadow, &shadow);
       d.dc->CreateEffect(CLSID_D2D1Composite, &comp);
       bool ok = bmp && matrix && shadow && comp;
       if (ok) {
          D2D1_MATRIX_5X4_F m = {0.2f, 0.7f, 0.1f, 0, 0.3f, 0.3f, 0.4f, 0, 0.6f, 0.1f, 0.3f, 0,
                                 0, 0, 0, 1, 0.05f, 0, 0.1f, 0};
          matrix->SetInput(0, bmp);
          matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
          shadow->SetInputEffect(0, matrix);
          shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 3.0f);
          comp->SetInputEffect(0, shadow);
          comp->SetInputEffect(1, matrix);
          D2D1_POINT_2F at = {12, 12};
          d.dc->DrawImage(comp, &at);
       }
       release(comp);
       release(shadow);
       release(matrix);
       release(bmp);
       return ok;
    }},
   {"Geometry realizations (Direct2D 1.2)", [](D2D &d, std::string &e) {
       ID2D1DeviceContext1 *dc1 = nullptr;
       d.dc->QueryInterface(__uuidof(ID2D1DeviceContext1), (void **)&dc1);
       if (!dc1) {
          e = "no ID2D1DeviceContext1 (optional)";
          return false;
       }
       ID2D1PathGeometry *path = wavePath(d);
       ID2D1GeometryRealization *fill = nullptr, *stroke = nullptr;
       if (path) {
          dc1->CreateFilledGeometryRealization(path, D2D1_DEFAULT_FLATTENING_TOLERANCE, &fill);
          dc1->CreateStrokedGeometryRealization(path, D2D1_DEFAULT_FLATTENING_TOLERANCE, 2, nullptr, &stroke);
       }
       ID2D1SolidColorBrush *b = solid(d, 0.3f, 0.9f, 0.4f), *b2 = solid(d, 0.1f, 0.2f, 0.1f);
       bool ok = fill && stroke && b && b2;
       if (ok) {
          dc1->DrawGeometryRealization(fill, b);
          dc1->DrawGeometryRealization(stroke, b2);
       }
       release(fill);
       release(stroke);
       release(path);
       release(b);
       release(b2);
       dc1->Release();
       return ok;
    }, true},
   {"Command lists (recorded, drawn twice)", [](D2D &d, std::string &e) {
       ID2D1CommandList *list = nullptr;
       d.dc->CreateCommandList(&list);
       ID2D1SolidColorBrush *b = solid(d, 0.7f, 0.2f, 0.9f);
       bool ok = list && b;
       if (ok) {
          ID2D1Image *old = nullptr;
          d.dc->GetTarget(&old);
          d.dc->SetTarget(list);
          d.dc->FillEllipse(D2D1_ELLIPSE{{10, 10}, 9, 6}, b);
          d.dc->DrawRectangle(D2D1_RECT_F{2, 2, 20, 18}, b, 1.5f);
          d.dc->SetTarget(old);
          release(old);
          list->Close();
          D2D1_POINT_2F a = {4, 6}, c = {30, 34};
          d.dc->DrawImage(list, &a);
          d.dc->DrawImage(list, &c);
       }
       release(b);
       release(list);
       return ok;
    }},
};

// The scene's pixels on one device: cleared, drawn, read back
static bool runD2D(D2D &d, const D2DTest &t, std::vector<uint32_t> &out, std::string &detail)
{
   d.dc->BeginDraw();
   d.dc->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
   d.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
   d.dc->Clear(D2D1_COLOR_F{0.95f, 0.95f, 0.9f, 1});
   bool ok = t.scene(d, detail);
   HRESULT h = d.dc->EndDraw();
   if (FAILED(h)) {
      detail = "EndDraw: " + hrs(h);
      return false;
   }
   return ok && readD2D(d, out);
}

// Direct2D 1.0's render target on a DXGI surface (the D3D10.1/11 interop of older apps)
static bool dxgiSurfaceTarget(D2D &d, std::vector<uint32_t> &out, std::string &detail)
{
   IDXGISurface *surface = nullptr;
   d.tex->QueryInterface(__uuidof(IDXGISurface), (void **)&surface);
   D2D1_RENDER_TARGET_PROPERTIES rp = {D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                       {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96,
                                       D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
   ID2D1RenderTarget *rt = nullptr;
   HRESULT h = surface ? d.factory->CreateDxgiSurfaceRenderTarget(surface, &rp, &rt) : E_NOINTERFACE;
   release(surface);
   if (FAILED(h)) {
      detail = "CreateDxgiSurfaceRenderTarget: " + hrs(h);
      return false;
   }
   ID2D1SolidColorBrush *b = nullptr;
   rt->CreateSolidColorBrush(D2D1_COLOR_F{0.2f, 0.4f, 0.8f, 1}, &b);
   rt->BeginDraw();
   rt->Clear(D2D1_COLOR_F{1, 1, 1, 1});
   rt->FillEllipse(D2D1_ELLIPSE{{32, 32}, 26, 15}, b);
   rt->DrawLine(D2D1_POINT_2F{4, 60}, D2D1_POINT_2F{60, 4}, b, 2.5f);
   h = rt->EndDraw();
   release(b);
   rt->Release();
   if (FAILED(h)) {
      detail = "EndDraw: " + hrs(h);
      return false;
   }
   return readD2D(d, out);
}

// GDI drawing on a Direct2D target (ID2D1GdiInteropRenderTarget: GetDC in the middle of a frame)
static bool gdiInterop(D2D &d, std::vector<uint32_t> &out, std::string &detail)
{
   ID2D1GdiInteropRenderTarget *gdi = nullptr;
   d.dc->QueryInterface(__uuidof(ID2D1GdiInteropRenderTarget), (void **)&gdi);
   if (!gdi) {
      detail = "no ID2D1GdiInteropRenderTarget";
      return false;
   }
   ID2D1SolidColorBrush *b = solid(d, 0.9f, 0.5f, 0.1f);
   d.dc->BeginDraw();
   d.dc->Clear(D2D1_COLOR_F{0.1f, 0.1f, 0.2f, 1});
   d.dc->FillEllipse(D2D1_ELLIPSE{{20, 20}, 16, 12}, b);
   HDC dc = nullptr;
   HRESULT h = gdi->GetDC(D2D1_DC_INITIALIZE_MODE_COPY, &dc);
   if (SUCCEEDED(h)) {
      HBRUSH brush = CreateSolidBrush(RGB(30, 200, 90));
      RECT r = {30, 30, 58, 54};
      FillRect(dc, &r, brush);
      DeleteObject(brush);
      RECT none = {};
      gdi->ReleaseDC(&none);
   } else {
      detail = "GetDC: " + hrs(h);
   }
   d.dc->FillRectangle(D2D1_RECT_F{4, 44, 24, 60}, b);
   HRESULT end = d.dc->EndDraw();
   release(b);
   gdi->Release();
   if (FAILED(h)) return false;
   if (FAILED(end)) {
      detail = "EndDraw: " + hrs(end);
      return false;
   }
   return readD2D(d, out);
}

// A 32-bit BMP of a scene (top-down), for looking at differences
static void saveBmp(const std::string &path, const std::vector<uint32_t> &px, UINT w, UINT h)
{
   FILE *f = fopen(path.c_str(), "wb");
   if (!f) return;
   BITMAPFILEHEADER fh = {};
   BITMAPINFOHEADER ih = {};
   fh.bfType = 0x4d42;
   fh.bfOffBits = sizeof fh + sizeof ih;
   fh.bfSize = fh.bfOffBits + w * h * 4;
   ih.biSize = sizeof ih;
   ih.biWidth = (LONG)w;
   ih.biHeight = -(LONG)h;
   ih.biPlanes = 1;
   ih.biBitCount = 32;
   fwrite(&fh, sizeof fh, 1, f);
   fwrite(&ih, sizeof ih, 1, f);
   fwrite(px.data(), 4, w * h, f);
   fclose(f);
}

// The GPU's pixels against WARP's: antialiased edges may differ a little, the rest must not. Failures leave both
// images next to the report (d2d-<test>-gpu.bmp, -warp.bmp).
static void compareD2D(const char *name, const std::vector<uint32_t> &a, const std::vector<uint32_t> &b,
                       const std::string &detail)
{
   int bad = 0, firstBad = -1, maxDiff = 0, farOff = 0;
   for (size_t i = 0; i < a.size() && i < b.size(); i++) {
      int diff = 0;
      for (int c = 0; c < 32; c += 8) diff = std::max(diff, std::abs((int)((a[i] >> c) & 255) - (int)((b[i] >> c) & 255)));
      maxDiff = std::max(maxDiff, diff);
      if (diff > 6) {
         if (firstBad < 0) firstBad = (int)i;
         bad++;
      }
      farOff += diff > 48;
   }
   // Antialiased edges come out a little different (the GPU's and WARP's coverage of the same sample pattern):
   // at most 8% of the pixels a little apart. Far off: at most 4 pixels, those whose centers lie exactly on an
   // aliased edge (ties go by GL's rule on the host, which differs from D3D's top-left rule for some slopes).
   bool ok = a.size() == b.size() && bad * 100 <= 8 * (int)a.size() && farOff <= 4;
   std::string why = ok ? (bad ? fmt("%d edge pixels differ by up to %d%s", bad, maxDiff,
                                     farOff ? fmt(" (%d on exact edge ties)", farOff).c_str() : "")
                               : detail)
                        : fmt("%d of %zu pixels differ from WARP's, by up to %d (first: #%d, 0x%08x vs 0x%08x)", bad,
                              a.size(), maxDiff, firstBad, firstBad >= 0 ? a[firstBad] : 0, firstBad >= 0 ? b[firstBad] : 0);
   if (!ok) {
      std::string file;
      for (const char *c = name; *c && file.size() < 40; c++) file += isalnum((unsigned char)*c) ? *c : '_';
      std::string base = g_outDir + "\\d2d-" + file + (sizeof(void *) == 4 ? "-32" : "");
      saveBmp(base + "-gpu.bmp", a, D2W, D2H);
      saveBmp(base + "-warp.bmp", b, D2W, D2H);
   }
   check(ok, name, why);
}

static void groupD2D()
{
   D2D hw, warp;
   std::string hwDetail, warpDetail;
   bool hw_ok = setupD2D(hw, g_warp, hwDetail);
   // No Direct2D 1.1 on this Windows (7 without its Platform Update): not the GPU's doing
   if (!hw_ok && hwDetail.rfind("D2D1CreateFactory", 0) == 0) {
      missing("Direct2D 1.1 device context on the GPU's Direct3D 11 device",
              hwDetail + " (Direct2D 1.1: Windows 8 and later, Windows 7 with its Platform Update, KB2670838)");
   } else {
      check(hw_ok, "Direct2D 1.1 device context on the GPU's Direct3D 11 device", hwDetail);
   }
   if (!setupD2D(warp, true, warpDetail)) {
      report(Status::Skip, "The tests of this group", "WARP (the reference) failed: " + warpDetail);
      hw.release();
      warp.release();
      return;
   }
   for (const D2DTest &t : D2D_TESTS) {
      if (!startTest(t.name)) continue;
      std::vector<uint32_t> a, b;
      std::string detail, wd;
      bool refOk = runD2D(warp, t, b, wd);
      bool ok = hw_ok && runD2D(hw, t, a, detail);
      if (!refOk) {
         report(Status::Skip, t.name, "WARP failed: " + wd);
         continue;
      }
      if (!ok) {
         if (t.optionalFeature) optional(false, t.name, detail.empty() ? "not supported (optional)" : detail);
         else check(false, t.name, detail.empty() ? "the GPU run failed" : detail);
         continue;
      }
      compareD2D(t.name, a, b, detail);
   }
   const struct {
      const char *name;
      bool (*fn)(D2D &, std::vector<uint32_t> &, std::string &);
   } others[] = {{"Direct2D 1.0 render target on a DXGI surface", dxgiSurfaceTarget},
                 {"GDI drawing on a Direct2D target (GDI interop)", gdiInterop}};
   for (const auto &o : others) {
      if (!startTest(o.name)) continue;
      std::vector<uint32_t> a, b;
      std::string detail, wd;
      if (!o.fn(warp, b, wd)) {
         report(Status::Skip, o.name, "WARP failed: " + wd);
         continue;
      }
      if (!hw_ok || !o.fn(hw, a, detail)) {
         check(false, o.name, detail.empty() ? "the GPU run failed" : detail);
         continue;
      }
      compareD2D(o.name, a, b, detail);
   }
   hw.release();
   warp.release();
}
