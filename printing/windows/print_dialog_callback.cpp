/*
 * Copyright (C) 2017, David PHAM-VAN <dev.nfet.net@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "print_dialog_callback.h"

#include <commdlg.h>
#include <ocidl.h>
#include <objbase.h>

namespace nfet {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PrintDialogCallback::PrintDialogCallback(std::vector<uint8_t> previewData)
    : previewData_(std::move(previewData)) {
  if (!previewData_.empty()) {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    pdfDoc_ = FPDF_LoadMemDocument64(previewData_.data(), previewData_.size(),
                                     nullptr);
  }
}

PrintDialogCallback::~PrintDialogCallback() {
  // Restore subclassed window proc before we disappear.
  if (previewHwnd_ && origWndProc_) {
    SetWindowLongPtr(previewHwnd_, GWLP_WNDPROC,
                     reinterpret_cast<LONG_PTR>(origWndProc_));
    SetWindowLongPtr(previewHwnd_, GWLP_USERDATA, 0);
  }
  if (pdfDoc_) {
    FPDF_CloseDocument(pdfDoc_);
    FPDF_DestroyLibrary();
  }
  if (site_) {
    site_->Release();
  }
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE PrintDialogCallback::QueryInterface(REFIID riid,
                                                              void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown) {
    *ppv = static_cast<IUnknown*>(static_cast<IPrintDialogCallback*>(this));
  } else if (riid == IID_IPrintDialogCallback) {
    *ppv = static_cast<IPrintDialogCallback*>(this);
  } else if (riid == IID_IObjectWithSite) {
    *ppv = static_cast<IObjectWithSite*>(this);
  } else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE PrintDialogCallback::AddRef() {
  return ++refCount_;
}

ULONG STDMETHODCALLTYPE PrintDialogCallback::Release() {
  ULONG ref = --refCount_;
  if (ref == 0) delete this;
  return ref;
}

// ---------------------------------------------------------------------------
// IObjectWithSite
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE PrintDialogCallback::SetSite(IUnknown* pUnkSite) {
  if (site_) { site_->Release(); site_ = nullptr; }
  if (pUnkSite) { site_ = pUnkSite; site_->AddRef(); }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PrintDialogCallback::GetSite(REFIID riid,
                                                       void** ppvSite) {
  if (!site_) return E_FAIL;
  return site_->QueryInterface(riid, ppvSite);
}

// ---------------------------------------------------------------------------
// IPrintDialogCallback
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE PrintDialogCallback::InitDone() {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PrintDialogCallback::SelectionChange() {
  // Trigger a repaint — the subclassed WndProc does the actual rendering.
  if (previewHwnd_) {
    InvalidateRect(previewHwnd_, nullptr, TRUE);
  }
  return S_OK;
}

// HandleMessage is called for messages sent to the General-tab child dialog.
// We hook WM_INITDIALOG to find and subclass the preview Static control.
HRESULT STDMETHODCALLTYPE
PrintDialogCallback::HandleMessage(HWND hDlg,
                                   UINT uMsg,
                                   WPARAM /*wParam*/,
                                   LPARAM /*lParam*/,
                                   LRESULT* /*pResult*/) {
  if (uMsg == WM_INITDIALOG && !previewHwnd_ && pdfDoc_) {
    previewHwnd_ = findPreviewStatic(hDlg);
    if (previewHwnd_) {
      // Store 'this' in the window's user data so PreviewWndProc can reach us.
      SetWindowLongPtr(previewHwnd_, GWLP_USERDATA,
                       reinterpret_cast<LONG_PTR>(this));
      // Subclass: replace the window proc.
      origWndProc_ = reinterpret_cast<WNDPROC>(
          SetWindowLongPtr(previewHwnd_, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR>(&PreviewWndProc)));
      // Force an immediate repaint.
      InvalidateRect(previewHwnd_, nullptr, TRUE);
    }
  }
  // S_FALSE = let the dialog handle all other messages normally.
  return S_FALSE;
}

// ---------------------------------------------------------------------------
// Subclass WndProc for the preview Static control
// ---------------------------------------------------------------------------

LRESULT CALLBACK PrintDialogCallback::PreviewWndProc(HWND hwnd, UINT msg,
                                                     WPARAM wParam,
                                                     LPARAM lParam) {
  auto* self = reinterpret_cast<PrintDialogCallback*>(
      GetWindowLongPtr(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_ERASEBKGND:
      return 1;  // suppress default erase to avoid flicker

    case WM_PAINT:
      if (self && self->pdfDoc_) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        self->renderInto(hdc, hwnd);
        EndPaint(hwnd, &ps);
        return 0;
      }
      break;
  }

  return self ? CallWindowProc(self->origWndProc_, hwnd, msg, wParam, lParam)
              : DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void PrintDialogCallback::renderInto(HDC hdc, HWND hwnd) {
  if (!pdfDoc_) return;

  auto page = FPDF_LoadPage(pdfDoc_, 0);
  if (!page) return;

  RECT rc;
  GetClientRect(hwnd, &rc);
  const int panelW = rc.right - rc.left;
  const int panelH = rc.bottom - rc.top;

  const double pdfW = FPDF_GetPageWidth(page);
  const double pdfH = FPDF_GetPageHeight(page);

  if (panelW <= 0 || panelH <= 0 || pdfW <= 0 || pdfH <= 0) {
    FPDF_ClosePage(page);
    return;
  }

  // Scale to fit, preserve aspect ratio, leave a small margin.
  const int margin = 8;
  const double scaleX = (panelW - 2 * margin) / pdfW;
  const double scaleY = (panelH - 2 * margin) / pdfH;
  const double scale = (scaleX < scaleY) ? scaleX : scaleY;

  const int bmpW = static_cast<int>(pdfW * scale);
  const int bmpH = static_cast<int>(pdfH * scale);

  // Render into an FPDF_BITMAP (BGRA).
  auto bitmap = FPDFBitmap_Create(bmpW, bmpH, 1);
  if (!bitmap) { FPDF_ClosePage(page); return; }
  FPDFBitmap_FillRect(bitmap, 0, 0, bmpW, bmpH, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, bmpW, bmpH, 0, FPDF_ANNOT);

  // Fill background.
  HBRUSH bgBrush = CreateSolidBrush(RGB(230, 230, 230));
  FillRect(hdc, &rc, bgBrush);
  DeleteObject(bgBrush);

  // Draw a white shadow border around the page.
  const int pageX = (panelW - bmpW) / 2;
  const int pageY = (panelH - bmpH) / 2;
  RECT pageRect = {pageX - 1, pageY - 1, pageX + bmpW + 1, pageY + bmpH + 1};
  FrameRect(hdc, &pageRect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

  // Blit PDFium pixels.
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = bmpW;
  bmi.bmiHeader.biHeight = -bmpH;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  StretchDIBits(hdc, pageX, pageY, bmpW, bmpH, 0, 0, bmpW, bmpH,
                FPDFBitmap_GetBuffer(bitmap), &bmi, DIB_RGB_COLORS, SRCCOPY);

  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);
}

// ---------------------------------------------------------------------------
// Find the preview Static control
// ---------------------------------------------------------------------------

HWND PrintDialogCallback::findPreviewStatic(HWND dlgHwnd) {
  // The preview area in the PrintDlgEx General-tab child dialog is a Static
  // control. We find it by looking for the Static-class child with the largest
  // area (they're typically much bigger than label statics).
  struct State {
    HWND best = nullptr;
    LONG bestArea = 0;
  } state;

  EnumChildWindows(
      dlgHwnd,
      [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* s = reinterpret_cast<State*>(lParam);

        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (_wcsicmp(cls, L"Static") != 0) return TRUE;

        RECT r;
        GetWindowRect(hwnd, &r);
        LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (area > s->bestArea) {
          s->bestArea = area;
          s->best = hwnd;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&state));

  return state.best;
}

}  // namespace nfet
