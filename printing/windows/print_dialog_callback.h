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

#ifndef PRINTING_PLUGIN_PRINT_DIALOG_CALLBACK_H_
#define PRINTING_PLUGIN_PRINT_DIALOG_CALLBACK_H_

#include <windows.h>
#include <commdlg.h>
#include <ocidl.h>   // IObjectWithSite
#include <fpdfview.h>

#include <vector>

namespace nfet {

// COM implementation of IPrintDialogCallback + IObjectWithSite.
// Provides live print preview inside the PrintDlgEx dialog by rendering
// page 1 of the supplied PDF document via PDFium.
//
// Usage:
//   PrintDialogCallback* cb = new PrintDialogCallback(pdfBytes);
//   pdx.lpCallback = static_cast<IUnknown*>(
//       static_cast<IPrintDialogCallback*>(cb));
//   PrintDlgEx(&pdx);
//   cb->Release();  // or let it go when done
class PrintDialogCallback : public IPrintDialogCallback,
                            public IObjectWithSite {
 public:
  // Takes ownership of the raw PDF bytes for preview rendering.
  // Pass an empty vector to disable preview.
  explicit PrintDialogCallback(std::vector<uint8_t> previewData);
  ~PrintDialogCallback();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IPrintDialogCallback
  // Called once the dialog is fully initialised — render the first preview.
  HRESULT STDMETHODCALLTYPE InitDone() override;
  // Called when the user changes printer or settings — re-render.
  HRESULT STDMETHODCALLTYPE SelectionChange() override;
  // Pass-through — return S_FALSE to let the dialog handle all messages.
  HRESULT STDMETHODCALLTYPE HandleMessage(HWND hDlg,
                                          UINT uMsg,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          LRESULT* pResult) override;

  // IObjectWithSite — the dialog calls SetSite to hand us IPrintDialogServices.
  HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override;
  HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) override;

 private:
  ULONG refCount_ = 1;
  IUnknown* site_ = nullptr;         // held from SetSite
  HWND previewHwnd_ = nullptr;       // child preview window inside the dialog
  FPDF_DOCUMENT pdfDoc_ = nullptr;   // loaded once, kept for lifetime of dialog

  std::vector<uint8_t> previewData_;

  // Renders page 0 of pdfDoc_ into previewHwnd_.
  void renderPreview();

  // Returns the preview child window from the dialog (best-effort search).
  static HWND findPreviewWindow(HWND dlgHwnd);
};

}  // namespace nfet

#endif  // PRINTING_PLUGIN_PRINT_DIALOG_CALLBACK_H_
