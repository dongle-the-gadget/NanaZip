﻿// ProgressDialog2.cpp
#pragma warning(disable: 4505)

#include "StdAfx.h"

#include <dwmapi.h>

#include "../../../Common/IntToString.h"
#include "../../../Common/StringConvert.h"

#include "../../../Windows/Clipboard.h"
#include "../../../Windows/ErrorMsg.h"

#include "../GUI/ExtractRes.h"

#include "LangUtils.h"

#include "DialogSize.h"
#include "ProgressDialog2.h"
#include "ProgressDialog2Res.h"

#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <Mile.Helpers.CppWinRT.h>
#include <Mile.Helpers.h>

// TODO: Implement messages (_messageList)

using namespace NWindows;

extern HINSTANCE g_hInstance;

static const UINT_PTR kTimerID = 3;

static const UINT kCloseMessage = WM_APP + 1;
// we can't use WM_USER, since WM_USER can be used by standard Windows procedure for Dialog

static const UINT kTimerElapse =
  #ifdef UNDER_CE
  500
  #else
  200
  #endif
  ;

static const UINT kCreateDelay =
  #ifdef UNDER_CE
  2500
  #else
  500
  #endif
  ;

static const DWORD kPauseSleepTime = 100;


#define UNDEFINED_VAL ((UInt64)(Int64)-1)
#define INIT_AS_UNDEFINED(v) v = UNDEFINED_VAL;
#define IS_UNDEFINED_VAL(v) ((v) == UNDEFINED_VAL)
#define IS_DEFINED_VAL(v) ((v) != UNDEFINED_VAL)

CProgressSync::CProgressSync():
    _stopped(false), _paused(false),
    _bytesProgressMode(true),
    _totalBytes(UNDEFINED_VAL), _completedBytes(0),
    _totalFiles(UNDEFINED_VAL), _curFiles(0),
    _inSize(UNDEFINED_VAL),
    _outSize(UNDEFINED_VAL),
    _isDir(false)
    {}

#define CHECK_STOP  if (_stopped) return E_ABORT; if (!_paused) return S_OK;
#define CRITICAL_LOCK NSynchronization::CCriticalSectionLock lock(_cs);

bool CProgressSync::Get_Paused()
{
  CRITICAL_LOCK
  return _paused;
}

HRESULT CProgressSync::CheckStop()
{
  for (;;)
  {
    {
      CRITICAL_LOCK
      CHECK_STOP
    }
    ::Sleep(kPauseSleepTime);
  }
}

HRESULT CProgressSync::ScanProgress(UInt64 numFiles, UInt64 totalSize, const FString &fileName, bool isDir)
{
  {
    CRITICAL_LOCK
    _totalFiles = numFiles;
    _totalBytes = totalSize;
    _filePath = fs2us(fileName);
    _isDir = isDir;
    // _completedBytes = 0;
    CHECK_STOP
  }
  return CheckStop();
}

HRESULT CProgressSync::Set_NumFilesTotal(UInt64 val)
{
  {
    CRITICAL_LOCK
    _totalFiles = val;
    CHECK_STOP
  }
  return CheckStop();
}

void CProgressSync::Set_NumBytesTotal(UInt64 val)
{
  CRITICAL_LOCK
  _totalBytes = val;
}

void CProgressSync::Set_NumFilesCur(UInt64 val)
{
  CRITICAL_LOCK
  _curFiles = val;
}

HRESULT CProgressSync::Set_NumBytesCur(const UInt64 *val)
{
  {
    CRITICAL_LOCK
    if (val)
      _completedBytes = *val;
    CHECK_STOP
  }
  return CheckStop();
}

HRESULT CProgressSync::Set_NumBytesCur(UInt64 val)
{
  {
    CRITICAL_LOCK
    _completedBytes = val;
    CHECK_STOP
  }
  return CheckStop();
}

void CProgressSync::Set_Ratio(const UInt64 *inSize, const UInt64 *outSize)
{
  CRITICAL_LOCK
  if (inSize)
    _inSize = *inSize;
  if (outSize)
    _outSize = *outSize;
}

void CProgressSync::Set_TitleFileName(const UString &fileName)
{
  CRITICAL_LOCK
  _titleFileName = fileName;
}

void CProgressSync::Set_Status(const UString &s)
{
  CRITICAL_LOCK
  _status = s;
}

HRESULT CProgressSync::Set_Status2(const UString &s, const wchar_t *path, bool isDir)
{
  {
    CRITICAL_LOCK
    _status = s;
    if (path)
      _filePath = path;
    else
      _filePath.Empty();
    _isDir = isDir;
  }
  return CheckStop();
}

void CProgressSync::Set_FilePath(const wchar_t *path, bool isDir)
{
  CRITICAL_LOCK
  if (path)
    _filePath = path;
  else
    _filePath.Empty();
  _isDir = isDir;
}


void CProgressSync::AddError_Message(const wchar_t *message)
{
  CRITICAL_LOCK
  Messages.Add(message);
}

void CProgressSync::AddError_Message_Name(const wchar_t *message, const wchar_t *name)
{
  UString s;
  if (name && *name != 0)
    s += name;
  if (message && *message != 0)
  {
    if (!s.IsEmpty())
      s.Add_LF();
    s += message;
    if (!s.IsEmpty() && s.Back() == L'\n')
      s.DeleteBack();
  }
  AddError_Message(s);
}

void CProgressSync::AddError_Code_Name(DWORD systemError, const wchar_t *name)
{
  UString s = NError::MyFormatMessage(systemError);
  if (systemError == 0)
    s = "Error";
  AddError_Message_Name(s, name);
}

CProgressDialog::CProgressDialog():
   _timer(0),
   CompressingMode(true),
   MainWindow(NULL)
{
  _isDir = false;

  _numMessages = 0;
  IconID = -1;
  MessagesDisplayed = false;
  _wasCreated = false;
  _needClose = false;
  _inCancelMessageBox = false;
  _externalCloseMessageWasReceived = false;

  _numPostedMessages = 0;
  _numAutoSizeMessages = 0;
  _errorsWereDisplayed = false;
  _waitCloseByCancelButton = false;
  _cancelWasPressed = false;
  ShowCompressionInfo = true;
  WaitMode = false;
  if (_dialogCreatedEvent.Create() != S_OK)
    throw 1334987;
  if (_createDialogEvent.Create() != S_OK)
    throw 1334987;
  #ifdef __ITaskbarList3_INTERFACE_DEFINED__
  _taskbarList = winrt::create_instance<ITaskbarList3>(
      CLSID_TaskbarList,
      CLSCTX_INPROC_SERVER);
  if (_taskbarList)
    _taskbarList->HrInit();
  #endif
}

#ifndef _SFX

CProgressDialog::~CProgressDialog()
{
  #ifdef __ITaskbarList3_INTERFACE_DEFINED__
  SetTaskbarProgressState(TBPF_NOPROGRESS);
  #endif
  AddToTitle(L"");
}
void CProgressDialog::AddToTitle(LPCWSTR s)
{
  if (MainWindow)
  {
    CWindow window(MainWindow);
    window.SetText((UString)s + MainTitle);
  }
}

#endif


void CProgressDialog::SetTaskbarProgressState()
{
  #ifdef __ITaskbarList3_INTERFACE_DEFINED__
  if (_taskbarList && _hwndForTaskbar)
  {
    TBPFLAG tbpFlags;
    if (Sync.Get_Paused())
      tbpFlags = TBPF_PAUSED;
    else
      tbpFlags = _errorsWereDisplayed ? TBPF_ERROR: TBPF_NORMAL;
    SetTaskbarProgressState(tbpFlags);
  }
  #endif
}

static const unsigned kTitleFileNameSizeLimit = 36;
static const unsigned kCurrentFileNameSizeLimit = 82;

static void ReduceString(UString &s, unsigned size)
{
  if (s.Len() <= size)
    return;
  s.Delete(size / 2, s.Len() - size);
  s.Insert(size / 2, L" ... ");
}

bool CProgressDialog::OnInit()
{
  _hwndForTaskbar = MainWindow;
  if (!_hwndForTaskbar)
    _hwndForTaskbar = GetParent();
  if (!_hwndForTaskbar)
    _hwndForTaskbar = *this;

  MileAllowNonClientDefaultDrawingForWindow(*this, FALSE);

  MARGINS margins = { -1 };
  DwmExtendFrameIntoClientArea(*this, &margins);

  INIT_AS_UNDEFINED(_progressBar_Range);
  INIT_AS_UNDEFINED(_progressBar_Pos);

  INIT_AS_UNDEFINED(_prevPercentValue);
  INIT_AS_UNDEFINED(_prevElapsedSec);
  INIT_AS_UNDEFINED(_prevRemainingSec);

  INIT_AS_UNDEFINED(_prevSpeed);
  _prevSpeed_MoveBits = 0;

  _prevTime = ::GetTickCount();
  _elapsedTime = 0;

  INIT_AS_UNDEFINED(_totalBytes_Prev);
  INIT_AS_UNDEFINED(_processed_Prev);
  INIT_AS_UNDEFINED(_packed_Prev);
  INIT_AS_UNDEFINED(_ratio_Prev);

  _filesStr_Prev.Empty();
  _filesTotStr_Prev.Empty();

  _foreground = true;

  m_progressPage = {};
  m_islandsHwnd = CreateWindowEx(
      WS_EX_NOREDIRECTIONBITMAP,
      L"Mile.Xaml.ContentWindow",
      nullptr,
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
      0, 0, 0, 0,
      *this,
      nullptr,
      nullptr,
      winrt::get_abi(m_progressPage));

  ::SetWindowSubclass(
      m_islandsHwnd,
      [](
          _In_ HWND hWnd,
          _In_ UINT uMsg,
          _In_ WPARAM wParam,
          _In_ LPARAM lParam,
          _In_ UINT_PTR uIdSubclass,
          _In_ DWORD_PTR dwRefData) -> LRESULT
      {
          UNREFERENCED_PARAMETER(uIdSubclass);
          UNREFERENCED_PARAMETER(dwRefData);

          switch (uMsg)
          {
          case WM_DESTROY:
          {
              winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource XamlSource = nullptr;
              winrt::copy_from_abi(
                  XamlSource,
                  ::GetPropW(hWnd, L"XamlWindowSource"));
              XamlSource.Close();
          }
          case WM_ERASEBKGND:
              ::RemovePropW(
                  hWnd,
                  L"BackgroundFallbackColor");
              break;
          }

          return ::DefSubclassProc(
              hWnd,
              uMsg,
              wParam,
              lParam);
      },
      0,
      0);

  RECT rect;
  GetClientRect(&rect);
  SetWindowPos(
      m_islandsHwnd,
      nullptr,
      0, 0,
      RECT_SIZE_X(rect), RECT_SIZE_Y(rect),
      SWP_SHOWWINDOW
  );

  _wasCreated = true;
  _dialogCreatedEvent.Set();

  m_progressPage.CancelButtonText(
      ::Mile::WinRT::GetLocalizedString(
          L"NanaZip.Modern/ProgressPage/CancelButtonText"));

  m_progressPage.CancelButtonClicked({ this, &CProgressDialog::OnCancelButtonClicked });
  m_progressPage.PauseButtonClicked({ this, &CProgressDialog::OnPauseButtonClicked });
  m_progressPage.BackgroundButtonClicked({ this, &CProgressDialog::OnBackgroundButtonClicked });

  _background_String = UString(::Mile::WinRT::GetLocalizedString(
      L"NanaZip.Modern/ProgressPage/BackgroundButtonText"
  ).c_str());
  _backgrounded_String = _background_String;
  _foreground_String = UString(::Mile::WinRT::GetLocalizedString(
      L"NanaZip.Modern/ProgressPage/ForegroundButtonText"
  ).c_str());
  _pause_String = UString(::Mile::WinRT::GetLocalizedString(
      L"NanaZip.Modern/ProgressPage/PauseButtonText"
  ).c_str());
  _continue_String = UString(::Mile::WinRT::GetLocalizedString(
      L"NanaZip.Modern/ProgressPage/ContinueButtonText"
  ).c_str());
  _paused_String = UString(::Mile::WinRT::GetLocalizedString(
      L"NanaZip.Modern/ProgressPage/PausedText"
  ).c_str());

  SetText(_title);
  SetPauseText();
  SetPriorityText();

  _numReduceSymbols = kCurrentFileNameSizeLimit;
  // NormalizeSize(true);

  if (!ShowCompressionInfo)
  {
      /*
      HideItem(IDT_PROGRESS_PACKED);
      HideItem(IDT_PROGRESS_PACKED_VAL);
      HideItem(IDT_PROGRESS_RATIO);
      HideItem(IDT_PROGRESS_RATIO_VAL);
      */
      m_progressPage.ShowPackedValue(false);
      m_progressPage.ShowCompressionRatioValue(false);
  }

  if (IconID >= 0)
  {
    HICON icon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IconID));
    // SetIcon(ICON_SMALL, icon);
    SetIcon(ICON_BIG, icon);
  }
  _timer = SetTimer(kTimerID, kTimerElapse);
#ifdef UNDER_CE
  Foreground();
#endif

  CheckNeedClose();

  SetTaskbarProgressState();

  return CModalDialog::OnInit();
}

bool CProgressDialog::OnSize(WPARAM /* wParam */, int xSize, int ySize)
{
    return BOOLToBool(SetWindowPos(
        m_islandsHwnd,
        nullptr,
        0, 0, xSize, ySize,
        SWP_SHOWWINDOW));
}


void CProgressDialog::SetProgressRange(UInt64 range)
{
  if (range == _progressBar_Range)
    return;
  _progressBar_Range = range;
  INIT_AS_UNDEFINED(_progressBar_Pos);
  _progressConv.Init(range);
  // m_ProgressBar.SetRange32(0, _progressConv.Count(range));
  m_progressPage.ProgressBarMaximum(_progressConv.Count(range));
}

void CProgressDialog::SetProgressPos(UInt64 pos)
{
  if (pos >= _progressBar_Range ||
      pos <= _progressBar_Pos ||
      pos - _progressBar_Pos >= (_progressBar_Range >> 10))
  {
    // m_ProgressBar.SetPos(_progressConv.Count(pos));
    m_progressPage.ProgressBarValue(_progressConv.Count(pos));
    #ifdef __ITaskbarList3_INTERFACE_DEFINED__
    if (_taskbarList && _hwndForTaskbar)
      _taskbarList->SetProgressValue(_hwndForTaskbar, pos, _progressBar_Range);
    #endif
    _progressBar_Pos = pos;
  }
}

#define UINT_TO_STR_2(val) { s[0] = (wchar_t)('0' + (val) / 10); s[1] = (wchar_t)('0' + (val) % 10); s += 2; }

void GetTimeString(UInt64 timeValue, wchar_t *s);
void GetTimeString(UInt64 timeValue, wchar_t *s)
{
  UInt64 hours = timeValue / 3600;
  UInt32 seconds = (UInt32)(timeValue - hours * 3600);
  UInt32 minutes = seconds / 60;
  seconds %= 60;
  if (hours > 99)
  {
    ConvertUInt64ToString(hours, s);
    for (; *s != 0; s++);
  }
  else
  {
    UInt32 hours32 = (UInt32)hours;
    UINT_TO_STR_2(hours32);
  }
  *s++ = ':'; UINT_TO_STR_2(minutes);
  *s++ = ':'; UINT_TO_STR_2(seconds);
  *s = 0;
}

static void ConvertSizeToString(UInt64 v, wchar_t *s)
{
  Byte c = 0;
       if (v >= ((UInt64)100000 << 20)) { v >>= 30; c = 'G'; }
  else if (v >= ((UInt64)100000 << 10)) { v >>= 20; c = 'M'; }
  else if (v >= ((UInt64)100000 <<  0)) { v >>= 10; c = 'K'; }
  ConvertUInt64ToString(v, s);
  if (c != 0)
  {
    s += MyStringLen(s);
    *s++ = ' ';
    *s++ = c;
    *s++ = 'B';
    *s++ = 0;
  }
}

winrt::hstring CProgressDialog::ShowSize(UInt64 val, UInt64 &prev)
{
  if (val == prev)
    return L"";
  prev = val;
  wchar_t s[40];
  s[0] = 0;
  if (IS_DEFINED_VAL(val))
    ConvertSizeToString(val, s);
  return s;
}

static void GetChangedString(const UString &newStr, UString &prevStr, bool &hasChanged)
{
  hasChanged = !(prevStr == newStr);
  if (hasChanged)
    prevStr = newStr;
}

static unsigned GetPower32(UInt32 val)
{
  const unsigned kStart = 32;
  UInt32 mask = ((UInt32)1 << (kStart - 1));
  for (unsigned i = kStart;; i--)
  {
    if (i == 0 || (val & mask) != 0)
      return i;
    mask >>= 1;
  }
}

static unsigned GetPower64(UInt64 val)
{
  UInt32 high = (UInt32)(val >> 32);
  if (high == 0)
    return GetPower32((UInt32)val);
  return GetPower32(high) + 32;
}

static UInt64 MyMultAndDiv(UInt64 mult1, UInt64 mult2, UInt64 divider)
{
  unsigned pow1 = GetPower64(mult1);
  unsigned pow2 = GetPower64(mult2);
  while (pow1 + pow2 > 64)
  {
    if (pow1 > pow2) { pow1--; mult1 >>= 1; }
    else             { pow2--; mult2 >>= 1; }
    divider >>= 1;
  }
  UInt64 res = mult1 * mult2;
  if (divider != 0)
    res /= divider;
  return res;
}

void CProgressDialog::UpdateStatInfo(bool showAll)
{
  UInt64 total, completed, totalFiles, completedFiles, inSize, outSize;
  bool bytesProgressMode;

  bool titleFileName_Changed;
  bool curFilePath_Changed;
  bool status_Changed;
  unsigned numErrors;
  {
    NSynchronization::CCriticalSectionLock lock(Sync._cs);
    total = Sync._totalBytes;
    completed = Sync._completedBytes;
    totalFiles = Sync._totalFiles;
    completedFiles = Sync._curFiles;
    inSize = Sync._inSize;
    outSize = Sync._outSize;
    bytesProgressMode = Sync._bytesProgressMode;

    GetChangedString(Sync._titleFileName, _titleFileName, titleFileName_Changed);
    GetChangedString(Sync._filePath, _filePath, curFilePath_Changed);
    GetChangedString(Sync._status, _status, status_Changed);
    if (_isDir != Sync._isDir)
    {
      curFilePath_Changed = true;
      _isDir = Sync._isDir;
    }
    numErrors = Sync.Messages.Size();
  }

  UInt32 curTime = ::GetTickCount();

  const UInt64 progressTotal = bytesProgressMode ? total : totalFiles;
  const UInt64 progressCompleted = bytesProgressMode ? completed : completedFiles;
  {
    if (IS_UNDEFINED_VAL(progressTotal))
    {
      // SetPos(0);
      // SetRange(progressCompleted);
    }
    else
    {
      if (_progressBar_Pos != 0 || progressCompleted != 0 ||
          (_progressBar_Range == 0 && progressTotal != 0))
      {
        SetProgressRange(progressTotal);
        SetProgressPos(progressCompleted);
      }
    }
  }

  // ShowSize(IDT_PROGRESS_TOTAL_VAL, total, _totalBytes_Prev);
  winrt::hstring size = ShowSize(total, _totalBytes_Prev);
  if (size != L"")
    m_progressPage.TotalSizeText(size);

  _elapsedTime += (curTime - _prevTime);
  _prevTime = curTime;
  UInt64 elapsedSec = _elapsedTime / 1000;
  bool elapsedChanged = false;
  if (elapsedSec != _prevElapsedSec)
  {
    _prevElapsedSec = elapsedSec;
    elapsedChanged = true;
    wchar_t s[40];
    GetTimeString(elapsedSec, s);
    // SetItemText(IDT_PROGRESS_ELAPSED_VAL, s);
    m_progressPage.ElapsedTimeText(s);
  }

  bool needSetTitle = false;
  if (elapsedChanged || showAll)
  {
    if (numErrors > _numPostedMessages)
    {
      UpdateMessagesDialog();
      wchar_t s[32];
      ConvertUInt64ToString(numErrors, s);
      // SetItemText(IDT_PROGRESS_ERRORS_VAL, s);
      if (!_errorsWereDisplayed)
      {
        _errorsWereDisplayed = true;
        SetTaskbarProgressState();
      }
    }

    if (progressCompleted != 0)
    {
      if (IS_UNDEFINED_VAL(progressTotal))
      {
        if (IS_DEFINED_VAL(_prevRemainingSec))
        {
          INIT_AS_UNDEFINED(_prevRemainingSec);
          // SetItemText(IDT_PROGRESS_REMAINING_VAL, L"");
          m_progressPage.RemainingTimeText(L"");
        }
      }
      else
      {
        UInt64 remainingTime = 0;
        if (progressCompleted < progressTotal)
          remainingTime = MyMultAndDiv(_elapsedTime, progressTotal - progressCompleted, progressCompleted);
        UInt64 remainingSec = remainingTime / 1000;
        if (remainingSec != _prevRemainingSec)
        {
          _prevRemainingSec = remainingSec;
          wchar_t s[40];
          GetTimeString(remainingSec, s);
          // SetItemText(IDT_PROGRESS_REMAINING_VAL, s);
          m_progressPage.RemainingTimeText(s);
        }
      }
      {
        UInt64 elapsedTime = (_elapsedTime == 0) ? 1 : _elapsedTime;
        UInt64 v = (progressCompleted * 1000) / elapsedTime;
        Byte c = 0;
        unsigned moveBits = 0;
             if (v >= ((UInt64)10000 << 10)) { moveBits = 20; c = 'M'; }
        else if (v >= ((UInt64)10000 <<  0)) { moveBits = 10; c = 'K'; }
        v >>= moveBits;
        if (moveBits != _prevSpeed_MoveBits || v != _prevSpeed)
        {
          _prevSpeed_MoveBits = moveBits;
          _prevSpeed = v;
          wchar_t s[40];
          ConvertUInt64ToString(v, s);
          unsigned pos = MyStringLen(s);
          s[pos++] = ' ';
          if (moveBits != 0)
            s[pos++] = c;
          s[pos++] = 'B';
          s[pos++] = '/';
          s[pos++] = 's';
          s[pos++] = 0;
          // SetItemText(IDT_PROGRESS_SPEED_VAL, s);
          m_progressPage.SpeedText(s);
        }
      }
    }

    {
      UInt64 percent = 0;
      {
        if (IS_DEFINED_VAL(progressTotal))
        {
          percent = progressCompleted * 100;
          if (progressTotal != 0)
            percent /= progressTotal;
        }
      }
      if (percent != _prevPercentValue)
      {
        _prevPercentValue = percent;
        needSetTitle = true;
      }
    }

    {
      wchar_t s[64];

      ConvertUInt64ToString(completedFiles, s);
      if (_filesStr_Prev != s)
      {
        _filesStr_Prev = s;
        // SetItemText(IDT_PROGRESS_FILES_VAL, s);
        m_progressPage.FilesText(s);
      }

      s[0] = 0;
      if (IS_DEFINED_VAL(totalFiles))
      {
        MyStringCopy(s, L" / ");
        ConvertUInt64ToString(totalFiles, s + MyStringLen(s));
      }
      if (_filesTotStr_Prev != s)
      {
        _filesTotStr_Prev = s;
        // SetItemText(IDT_PROGRESS_FILES_TOTAL, s);
        m_progressPage.FilesText(s);
      }
    }

    const UInt64 packSize   = CompressingMode ? outSize : inSize;
    const UInt64 unpackSize = CompressingMode ? inSize : outSize;

    if (IS_UNDEFINED_VAL(unpackSize) &&
        IS_UNDEFINED_VAL(packSize))
    {
      // ShowSize(IDT_PROGRESS_PROCESSED_VAL, completed, _processed_Prev);
      // ShowSize(IDT_PROGRESS_PACKED_VAL, UNDEFINED_VAL, _packed_Prev);
      winrt::hstring processedSize = ShowSize(completed, _processed_Prev);
      if (processedSize != L"")
        m_progressPage.ProcessedText(processedSize);

      winrt::hstring packedSize = ShowSize(UNDEFINED_VAL, _packed_Prev);
      if (packedSize != L"")
        m_progressPage.PackedSizeText(packedSize);
    }
    else
    {
      // ShowSize(IDT_PROGRESS_PROCESSED_VAL, unpackSize, _processed_Prev);
      // ShowSize(IDT_PROGRESS_PACKED_VAL, packSize, _packed_Prev);
      winrt::hstring processedSize = ShowSize(unpackSize, _processed_Prev);
      if (processedSize != L"")
        m_progressPage.ProcessedText(processedSize);

      winrt::hstring packedSize = ShowSize(packSize, _packed_Prev);
      if (packedSize != L"")
        m_progressPage.PackedSizeText(packedSize);

      if (IS_DEFINED_VAL(packSize) &&
          IS_DEFINED_VAL(unpackSize) &&
          unpackSize != 0)
      {
        wchar_t s[32];
        UInt64 ratio = packSize * 100 / unpackSize;
        if (_ratio_Prev != ratio)
        {
          _ratio_Prev = ratio;
          ConvertUInt64ToString(ratio, s);
          MyStringCat(s, L"%");
          // SetItemText(IDT_PROGRESS_RATIO_VAL, s);
          m_progressPage.CompressionRatioText(s);
        }
      }
    }
  }

  if (needSetTitle || titleFileName_Changed)
  {
    m_progressPage.ActionText(_title.Ptr());
    SetTitleText();
  }

  /*
  if (status_Changed)
  {
    UString s = _status;
    ReduceString(s, _numReduceSymbols);
    SetItemText(IDT_PROGRESS_STATUS, _status);
  }
  */

  if (curFilePath_Changed)
  {
    UString s1, s2;
    if (_isDir)
      s1 = _filePath;
    else
    {
      int slashPos = _filePath.ReverseFind_PathSepar();
      if (slashPos >= 0)
      {
        s1.SetFrom(_filePath, (unsigned)(slashPos + 1));
        s2 = _filePath.Ptr((unsigned)(slashPos + 1));
      }
      else
        s2 = _filePath;
    }
    ReduceString(s1, _numReduceSymbols);
    ReduceString(s2, _numReduceSymbols);
    s1.Add_LF();
    s1 += s2;
    // SetItemText(IDT_PROGRESS_FILE_NAME, s1);
    m_progressPage.FileNameText(s1.Ptr());
  }
}

bool CProgressDialog::OnTimer(WPARAM /* timerID */, LPARAM /* callback */)
{
  if (Sync.Get_Paused())
    return true;
  CheckNeedClose();
  UpdateStatInfo(false);
  return true;
}

struct CWaitCursor
{
  HCURSOR _waitCursor;
  HCURSOR _oldCursor;
  CWaitCursor()
  {
    _waitCursor = LoadCursor(NULL, IDC_WAIT);
    if (_waitCursor != NULL)
      _oldCursor = SetCursor(_waitCursor);
  }
  ~CWaitCursor()
  {
    if (_waitCursor != NULL)
      SetCursor(_oldCursor);
  }
};

INT_PTR CProgressDialog::Create(const UString &title, NWindows::CThread &thread, HWND wndParent)
{
  INT_PTR res = 0;
  try
  {
    if (WaitMode)
    {
      CWaitCursor waitCursor;
      HANDLE h[] = { thread, _createDialogEvent };

      DWORD res2 = WaitForMultipleObjects(ARRAY_SIZE(h), h, FALSE, kCreateDelay);
      if (res2 == WAIT_OBJECT_0 && !Sync.ThereIsMessage())
        return 0;
    }
    _title = title;
    BIG_DIALOG_SIZE(360, 192);
    res = CModalDialog::Create(SIZED_DIALOG(IDD_PROGRESS), wndParent);
  }
  catch(...)
  {
    _wasCreated = true;
    _dialogCreatedEvent.Set();
  }
  thread.Wait_Close();
  if (!MessagesDisplayed)
    MessageBoxW(wndParent, L"Progress Error", L"NanaZip", MB_ICONERROR);
  return res;
}

bool CProgressDialog::OnExternalCloseMessage()
{
  // it doesn't work if there is MessageBox.
  // #ifdef __ITaskbarList3_INTERFACE_DEFINED__
  // SetTaskbarProgressState(TBPF_NOPROGRESS);
  // #endif
  // AddToTitle(L"Finished ");
  // SetText(L"Finished2 ");

  UpdateStatInfo(true);

  // SetItemText(IDCANCEL, LangString(IDS_CLOSE));
  // ::SendMessage(GetItem(IDCANCEL), BM_SETSTYLE, BS_DEFPUSHBUTTON, MAKELPARAM(TRUE, 0));
  // HideItem(IDB_PROGRESS_BACKGROUND);
  // HideItem(IDB_PAUSE);
  m_progressPage.CancelButtonText(
      ::Mile::WinRT::GetLocalizedString(
          L"NanaZip.Modern/ProgressPage/CloseButtonText"));
  m_progressPage.ShowBackgroundButton(false);
  m_progressPage.ShowPauseButton(false);

  ProcessWasFinished_GuiVirt();

  m_progressPage.ShowProgress(false);
  m_progressPage.ShowResults(true);

  bool thereAreMessages;
  CProgressFinalMessage fm;
  {
    NSynchronization::CCriticalSectionLock lock(Sync._cs);
    thereAreMessages = !Sync.Messages.IsEmpty();
    fm = Sync.FinalMessage;
  }

  bool needToShowMessages = thereAreMessages;

  if (!fm.ErrorMessage.Message.IsEmpty())
  {
    MessagesDisplayed = true;
    needToShowMessages = true;
    m_progressPage.ResultsText(fm.ErrorMessage.Message.Ptr());
  }
  else if (!thereAreMessages)
  {
    MessagesDisplayed = true;
    if (!fm.OkMessage.Message.IsEmpty())
    {
        needToShowMessages = true;
        m_progressPage.ResultsText(fm.OkMessage.Message.Ptr());
    }
  }

  if (needToShowMessages && !_cancelWasPressed)
  {
    _waitCloseByCancelButton = true;
    if (thereAreMessages)
        UpdateMessagesDialog();
    return true;
  }

  End(0);
  return true;
}

bool CProgressDialog::OnCancelClicked()
{
    if (_waitCloseByCancelButton)
    {
        MessagesDisplayed = true;
        End(IDCLOSE);
        return false;
    }

    if (_cancelWasPressed)
        return true;

    const bool paused = Sync.Get_Paused();

    if (!paused)
    {
        OnPauseButton();
    }

    _inCancelMessageBox = true;
    const int res = ::MessageBoxW(*this, LangString(IDS_PROGRESS_ASK_CANCEL), _title, MB_YESNOCANCEL);
    _inCancelMessageBox = false;
    if (res == IDYES)
        _cancelWasPressed = true;

    if (!paused)
    {
        OnPauseButton();
    }

    if (_externalCloseMessageWasReceived)
    {
        /* we have received kCloseMessage while we were in MessageBoxW().
           so we call OnExternalCloseMessage() here.
           it can show MessageBox and it can close dialog */
        OnExternalCloseMessage();
        return true;
    }

    if (!_cancelWasPressed)
        return true;

    MessagesDisplayed = true;
    // we will call Sync.Set_Stopped(true) in OnButtonClicked() : OnCancel()
    Sync.Set_Stopped(true);

    return false;
}

bool CProgressDialog::OnButtonClicked(int buttonID, HWND buttonHWND)
{
    switch (buttonID)
    {
        // case IDOK: // if IDCANCEL is not DEFPUSHBUTTON
    case IDCANCEL:
    {
        bool click = OnCancelClicked();
        if (click)
            return true;
        break;
    }
    }
    return CModalDialog::OnButtonClicked(buttonID, buttonHWND);
}

bool CProgressDialog::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
  {
    case kCloseMessage:
    {
      if (_timer)
      {
        /* 21.03 : KillTimer(kTimerID) instead of KillTimer(_timer).
           But (_timer == kTimerID) in Win10. So it worked too */
        KillTimer(kTimerID);
        _timer = 0;
      }
      if (_inCancelMessageBox)
      {
        /* if user is in MessageBox(), we will call OnExternalCloseMessage()
           later, when MessageBox() will be closed */
        _externalCloseMessageWasReceived = true;
        break;
      }
      return OnExternalCloseMessage();
    }
    /*
    case WM_SETTEXT:
    {
      if (_timer == 0)
        return true;
      break;
    }
    */
  }
  return CModalDialog::OnMessage(message, wParam, lParam);
}

void CProgressDialog::SetTitleText()
{
  UString s;
  if (Sync.Get_Paused())
  {
    s += _paused_String;
    s.Add_Space();
  }
  if (IS_DEFINED_VAL(_prevPercentValue))
  {
    char temp[32];
    ConvertUInt64ToString(_prevPercentValue, temp);
    s += temp;
    s += '%';
  }
  if (!_foreground)
  {
    s.Add_Space();
    s += _backgrounded_String;
  }

  s.Add_Space();
  #ifndef _SFX
  {
    unsigned len = s.Len();
    s += MainAddTitle;
    AddToTitle(s);
    s.DeleteFrom(len);
  }
  #endif

  s += _title;
  if (!_titleFileName.IsEmpty())
  {
    UString fileName = _titleFileName;
    ReduceString(fileName, kTitleFileNameSizeLimit);
    s.Add_Space();
    s += fileName;
  }
  SetText(s);
}

void CProgressDialog::SetPauseText()
{
  // SetItemText(IDB_PAUSE, Sync.Get_Paused() ? _continue_String : _pause_String);
  m_progressPage.PauseButtonText(
      Sync.Get_Paused() ?
      _continue_String.Ptr() :
      _pause_String.Ptr());
  m_progressPage.ShowPaused(Sync.Get_Paused());
  SetTitleText();
}

void CProgressDialog::OnPauseButton()
{
  bool paused = !Sync.Get_Paused();
  Sync.Set_Paused(paused);
  UInt32 curTime = ::GetTickCount();
  if (paused)
    _elapsedTime += (curTime - _prevTime);
  SetTaskbarProgressState();
  _prevTime = curTime;
  SetPauseText();
}

void CProgressDialog::SetPriorityText()
{
  /*
  SetItemText(IDB_PROGRESS_BACKGROUND, _foreground ?
      _background_String :
      _foreground_String);
  */
  m_progressPage.BackgroundButtonText(
      _foreground ?
      _background_String.Ptr() :
      _foreground_String.Ptr());
  SetTitleText();
}

void CProgressDialog::OnPriorityButton()
{
  _foreground = !_foreground;
  #ifndef UNDER_CE
  SetPriorityClass(GetCurrentProcess(), _foreground ? NORMAL_PRIORITY_CLASS: IDLE_PRIORITY_CLASS);
  #endif
  SetPriorityText();
}

void CProgressDialog::AddMessageDirect(LPCWSTR message, bool needNumber)
{
  UNREFERENCED_PARAMETER(message);
  UNREFERENCED_PARAMETER(needNumber);
  /*
  wchar_t sz[16];
  sz[0] = 0;
  if (needNumber)
    ConvertUInt32ToString(_numMessages + 1, sz);
  const unsigned itemIndex = _messageStrings.Size(); // _messageList.GetItemCount();
  if (_messageList.InsertItem((int)itemIndex, sz) == (int)itemIndex)
  {
    _messageList.SetSubItem((int)itemIndex, 1, message);
    _messageStrings.Add(message);
  }
  */

  std::wstring current = std::wstring((std::wstring_view)m_progressPage.ResultsText());
  if (needNumber && current.length() != 0)
      current += L"------------------------\n";
  current += message;
  current += L"\n";
  m_progressPage.ResultsText(current);
}

void CProgressDialog::AddMessage(LPCWSTR message)
{
  UString s = message;
  bool needNumber = true;
  while (!s.IsEmpty())
  {
    int pos = s.Find(L'\n');
    if (pos < 0)
      break;
    AddMessageDirect(s.Left(pos), needNumber);
    needNumber = false;
    s.DeleteFrontal(pos + 1);
  }
  AddMessageDirect(s, needNumber);
  _numMessages++;
}

static unsigned GetNumDigits(UInt32 val)
{
  unsigned i;
  for (i = 0; val >= 10; i++)
    val /= 10;
  return i;
}

void CProgressDialog::UpdateMessagesDialog()
{
  UStringVector messages;
  {
    NSynchronization::CCriticalSectionLock lock(Sync._cs);
    unsigned num = Sync.Messages.Size();
    if (num > _numPostedMessages)
    {
      messages.ClearAndReserve(num - _numPostedMessages);
      for (unsigned i = _numPostedMessages; i < num; i++)
        messages.AddInReserved(Sync.Messages[i]);
      _numPostedMessages = num;
    }
  }
  if (!messages.IsEmpty())
  {
    FOR_VECTOR (i, messages)
      AddMessage(messages[i]);
    if (_numAutoSizeMessages < 256 || GetNumDigits(_numPostedMessages) > GetNumDigits(_numAutoSizeMessages))
    {
      _messageList.SetColumnWidthAuto(0);
      _messageList.SetColumnWidthAuto(1);
      _numAutoSizeMessages = _numPostedMessages;
    }
  }
}

void CProgressDialog::OnCancelButtonClicked(
    winrt::IInspectable const&,
    winrt::RoutedEventArgs const&
)
{
    OnCancelClicked();
}

void CProgressDialog::OnPauseButtonClicked(
    winrt::IInspectable const&,
    winrt::RoutedEventArgs const&
)
{
    OnPauseButton();
}

void CProgressDialog::OnBackgroundButtonClicked(
    winrt::IInspectable const&,
    winrt::RoutedEventArgs const&
)
{
    OnPriorityButton();
}

void CProgressDialog::CheckNeedClose()
{
  if (_needClose)
  {
    PostMsg(kCloseMessage);
    _needClose = false;
  }
}

void CProgressDialog::ProcessWasFinished()
{
  // Set Window title here.
  if (!WaitMode)
    WaitCreating();

  if (_wasCreated)
    PostMsg(kCloseMessage);
  else
    _needClose = true;
}


static void ListView_GetSelected(NControl::CListView &listView, CUIntVector &vector)
{
  vector.Clear();
  int index = -1;
  for (;;)
  {
    index = listView.GetNextSelectedItem(index);
    if (index < 0)
      break;
    vector.Add(index);
  }
}


static THREAD_FUNC_DECL MyThreadFunction(void *param)
{
  CProgressThreadVirt *p = (CProgressThreadVirt *)param;
  try
  {
    p->Process();
    p->ThreadFinishedOK = true;
  }
  catch (...) { p->Result = E_FAIL; }
  return 0;
}


HRESULT CProgressThreadVirt::Create(const UString &title, HWND parentWindow)
{
  NWindows::CThread thread;
  RINOK(thread.Create(MyThreadFunction, this));
  CProgressDialog::Create(title, thread, parentWindow);
  return S_OK;
}

static void AddMessageToString(UString &dest, const UString &src)
{
  if (!src.IsEmpty())
  {
    if (!dest.IsEmpty())
      dest.Add_LF();
    dest += src;
  }
}

void CProgressThreadVirt::Process()
{
  CProgressCloser closer(*this);
  UString m;
  try { Result = ProcessVirt(); }
  catch(const wchar_t *s) { m = s; }
  catch(const UString &s) { m = s; }
  catch(const char *s) { m = GetUnicodeString(s); }
  catch(int v)
  {
    m = "Error #";
    m.Add_UInt32(v);
  }
  catch(...) { m = "Error"; }
  if (Result != E_ABORT)
  {
    if (m.IsEmpty() && Result != S_OK)
      m = HResultToMessage(Result);
  }
  AddMessageToString(m, FinalMessage.ErrorMessage.Message);

  {
    FOR_VECTOR(i, ErrorPaths)
    {
      if (i >= 32)
        break;
      AddMessageToString(m, fs2us(ErrorPaths[i]));
    }
  }

  CProgressSync &sync = Sync;
  NSynchronization::CCriticalSectionLock lock(sync._cs);
  if (m.IsEmpty())
  {
    if (!FinalMessage.OkMessage.Message.IsEmpty())
      sync.FinalMessage.OkMessage = FinalMessage.OkMessage;
  }
  else
  {
    sync.FinalMessage.ErrorMessage.Message = m;
    if (Result == S_OK)
      Result = E_FAIL;
  }
}

UString HResultToMessage(HRESULT errorCode)
{
  if (errorCode == E_OUTOFMEMORY)
    return LangString(IDS_MEM_ERROR);
  else
    return NError::MyFormatMessage(errorCode);
}
