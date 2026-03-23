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
    // Pre-load the PDF document so we can render quickly on SelectionChange.
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    pdfDoc_ = FPDF_LoadMemDocument64(previewData_.data(), previewData_.size(),
                                     nullptr);
  }
}

PrintDialogCallback::~PrintDialogCallback() {
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
    // Resolve ambiguity: prefer IPrintDialogCallback's IUnknown
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
  if (ref == 0) {
    delete this;
  }
  return ref;
}

// ---------------------------------------------------------------------------
// IObjectWithSite
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE PrintDialogCallback::SetSite(IUnknown* pUnkSite) {
  if (site_) {
    site_->Release();
    site_ = nullptr;
  }
  if (pUnkSite) {
    site_ = pUnkSite;
    site_->AddRef();
  }
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
  // Retrieve the dialog HWND via IPrintDialogServices and find the preview pane.
  if (site_) {
    IPrintDialogServices* svc = nullptr;
    if (SUCCEEDED(site_->QueryInterface(IID_IPrintDialogServices,
                                        reinterpret_cast<void**>(&svc)))) {
      // The dialog hasn't laid out yet — defer to SelectionChange which is
      // called right after InitDone when the printer selection is ready.
      svc->Release();
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PrintDialogCallback::SelectionChange() {
  if (!pdfDoc_ || !site_) return S_OK;

  // Find the preview HWND lazily.
  if (!previewHwnd_) {
    IPrintDialogServices* svc = nullptr;
    if (SUCCEEDED(site_->QueryInterface(IID_IPrintDialogServices,
                                        reinterpret_cast<void**>(&svc)))) {
      // Walk up from any window we know to find the dialog itself.
      // We use GetActiveWindow as a fallback heuristic — the dialog is modal.
      HWND dlgHwnd = GetActiveWindow();
      previewHwnd_ = findPreviewWindow(dlgHwnd);
      svc->Release();
    }
  }

  if (previewHwnd_) {
    renderPreview();
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE
PrintDialogCallback::HandleMessage(HWND /*hDlg*/,
                                   UINT /*uMsg*/,
                                   WPARAM /*wParam*/,
                                   LPARAM /*lParam*/,
                                   LRESULT* /*pResult*/) {
  // S_FALSE = let the dialog handle all messages normally.
  return S_FALSE;
}

// ---------------------------------------------------------------------------
// Preview rendering helpers
// ---------------------------------------------------------------------------

void PrintDialogCallback::renderPreview() {
  if (!pdfDoc_ || !previewHwnd_) return;

  auto page = FPDF_LoadPage(pdfDoc_, 0);
  if (!page) return;

  RECT rc;
  GetClientRect(previewHwnd_, &rc);
  const int panelW = rc.right - rc.left;
  const int panelH = rc.bottom - rc.top;
  if (panelW <= 0 || panelH <= 0) {
    FPDF_ClosePage(page);
    return;
  }

  // Scale the page to fit inside the panel while preserving aspect ratio.
  const double pdfW = FPDF_GetPageWidth(page);
  const double pdfH = FPDF_GetPageHeight(page);
  if (pdfW <= 0 || pdfH <= 0) {
    FPDF_ClosePage(page);
    return;
  }

  const double scaleX = panelW / pdfW;
  const double scaleY = panelH / pdfH;
  const double scale = (scaleX < scaleY) ? scaleX : scaleY;

  const int bmpW = static_cast<int>(pdfW * scale);
  const int bmpH = static_cast<int>(pdfH * scale);

  // Create an FPDF bitmap backed by a DIB section for direct GDI use.
  auto bitmap = FPDFBitmap_Create(bmpW, bmpH, 1 /* alpha */);
  if (!bitmap) {
    FPDF_ClosePage(page);
    return;
  }
  FPDFBitmap_FillRect(bitmap, 0, 0, bmpW, bmpH, 0xFFFFFFFF);  // white bg
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, bmpW, bmpH, 0, FPDF_ANNOT);

  // Build a BITMAPINFO for the raw BGRA data from PDFium.
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = bmpW;
  bmi.bmiHeader.biHeight = -bmpH;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  const int offsetX = (panelW - bmpW) / 2;
  const int offsetY = (panelH - bmpH) / 2;

  HDC hdc = GetDC(previewHwnd_);
  // Fill background.
  HBRUSH bgBrush = CreateSolidBrush(RGB(240, 240, 240));
  FillRect(hdc, &rc, bgBrush);
  DeleteObject(bgBrush);

  // Blit PDFium pixels.
  StretchDIBits(hdc, offsetX, offsetY, bmpW, bmpH, 0, 0, bmpW, bmpH,
                FPDFBitmap_GetBuffer(bitmap), &bmi, DIB_RGB_COLORS, SRCCOPY);

  ReleaseDC(previewHwnd_, hdc);

  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);
}

// Breadth-first search for the preview child window.
// PrintDlgEx embeds a preview area as a child window — its class name varies
// by Windows version but the panel is typically the tallest child by height.
// We pick the largest child window as a best-effort heuristic.
HWND PrintDialogCallback::findPreviewWindow(HWND dlgHwnd) {
  if (!dlgHwnd) return nullptr;

  // Walk all immediate children and pick the one with the largest area.
  struct SearchState {
    HWND best = nullptr;
    LONG bestArea = 0;
  } state;

  EnumChildWindows(
      dlgHwnd,
      [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* s = reinterpret_cast<SearchState*>(lParam);
        RECT r;
        if (GetClientRect(hwnd, &r)) {
          LONG area = (r.right - r.left) * (r.bottom - r.top);
          if (area > s->bestArea) {
            s->bestArea = area;
            s->best = hwnd;
          }
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&state));

  return state.best;
}

}  // namespace nfet
