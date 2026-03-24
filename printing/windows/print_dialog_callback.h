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
// Provides live print preview inside the PrintDlgEx dialog by subclassing
// the dialog's preview Static control and rendering page 1 via PDFium.
class PrintDialogCallback : public IPrintDialogCallback,
                            public IObjectWithSite {
 public:
  explicit PrintDialogCallback(std::vector<uint8_t> previewData);
  ~PrintDialogCallback();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IPrintDialogCallback
  HRESULT STDMETHODCALLTYPE InitDone() override;
  HRESULT STDMETHODCALLTYPE SelectionChange() override;
  HRESULT STDMETHODCALLTYPE HandleMessage(HWND hDlg, UINT uMsg,
                                          WPARAM wParam, LPARAM lParam,
                                          LRESULT* pResult) override;

  // IObjectWithSite
  HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override;
  HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) override;

 private:
  ULONG refCount_ = 1;
  IUnknown* site_ = nullptr;
  HWND previewHwnd_ = nullptr;
  WNDPROC origWndProc_ = nullptr;
  FPDF_DOCUMENT pdfDoc_ = nullptr;
  std::vector<uint8_t> previewData_;

  void renderInto(HDC hdc, HWND hwnd);
  static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam);
  static HWND findPreviewStatic(HWND dlgHwnd);
};

}  // namespace nfet

#endif  // PRINTING_PLUGIN_PRINT_DIALOG_CALLBACK_H_
