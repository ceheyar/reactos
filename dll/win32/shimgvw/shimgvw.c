/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Image file browsing and manipulation
 * COPYRIGHT:   Copyright Dmitry Chapyshev (dmitry@reactos.org)
 *              Copyright 2018-2023 Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */

#include "shimgvw.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

/* Toolbar image size */
#define TB_IMAGE_WIDTH  16
#define TB_IMAGE_HEIGHT 16

/* Slide show timer */
#define SLIDESHOW_TIMER_ID          0xFACE
#define SLIDESHOW_TIMER_INTERVAL    5000 /* 5 seconds */

HINSTANCE g_hInstance = NULL;
HWND g_hMainWnd = NULL;
HWND g_hwndFullscreen = NULL;
SHIMGVW_SETTINGS g_Settings;
SHIMGVW_FILENODE *g_pCurrentFile;
GpImage *g_pImage = NULL;

static const UINT s_ZoomSteps[] =
{
    5, 10, 25, 50, 100, 125, 200, 300, 500, 1000
};

#define MIN_ZOOM s_ZoomSteps[0]
#define MAX_ZOOM s_ZoomSteps[_countof(s_ZoomSteps) - 1]

    /* iBitmap,       idCommand,   fsState,         fsStyle,     bReserved[2], dwData, iString */
#define DEFINE_BTN_INFO(_name) \
    { TBICON_##_name, IDC_##_name, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0 }

#define DEFINE_BTN_SEPARATOR \
    { -1,             0,           TBSTATE_ENABLED, BTNS_SEP,    {0}, 0, 0 }

/* ToolBar Buttons */
static const TBBUTTON s_Buttons[] =
{
    DEFINE_BTN_INFO(PREV_PIC),
    DEFINE_BTN_INFO(NEXT_PIC),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(BEST_FIT),
    DEFINE_BTN_INFO(REAL_SIZE),
    DEFINE_BTN_INFO(SLIDE_SHOW),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(ZOOM_IN),
    DEFINE_BTN_INFO(ZOOM_OUT),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(ROT_CLOCKW),
    DEFINE_BTN_INFO(ROT_COUNCW),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(DELETE),
    DEFINE_BTN_INFO(PRINT),
    DEFINE_BTN_INFO(SAVEAS),
    DEFINE_BTN_INFO(MODIFY),
    DEFINE_BTN_SEPARATOR,
    DEFINE_BTN_INFO(HELP_TOC)
};

/* ToolBar Button configuration */
typedef struct
{
    DWORD idb;  /* Index to bitmap */
    DWORD ids;  /* Index to tooltip */
} TB_BUTTON_CONFIG;

#define DEFINE_BTN_CONFIG(_name) { IDB_##_name, IDS_TOOLTIP_##_name }

static const TB_BUTTON_CONFIG s_ButtonConfig[] =
{
    DEFINE_BTN_CONFIG(PREV_PIC),
    DEFINE_BTN_CONFIG(NEXT_PIC),
    DEFINE_BTN_CONFIG(BEST_FIT),
    DEFINE_BTN_CONFIG(REAL_SIZE),
    DEFINE_BTN_CONFIG(SLIDE_SHOW),
    DEFINE_BTN_CONFIG(ZOOM_IN),
    DEFINE_BTN_CONFIG(ZOOM_OUT),
    DEFINE_BTN_CONFIG(ROT_CLOCKW),
    DEFINE_BTN_CONFIG(ROT_COUNCW),
    DEFINE_BTN_CONFIG(DELETE),
    DEFINE_BTN_CONFIG(PRINT),
    DEFINE_BTN_CONFIG(SAVEAS),
    DEFINE_BTN_CONFIG(MODIFY),
    DEFINE_BTN_CONFIG(HELP_TOC)
};

typedef struct tagPREVIEW_DATA
{
    HWND m_hwnd;
    HWND m_hwndZoom;
    HWND m_hwndToolBar;
    INT m_nZoomPercents;
    WNDPROC m_fnPrevProc;
    ANIME m_Anime; /* Animation */
} PREVIEW_DATA, *PPREVIEW_DATA;

static inline PPREVIEW_DATA
Preview_GetData(HWND hwnd)
{
    return (PPREVIEW_DATA)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static inline BOOL
Preview_IsMainWnd(HWND hwnd)
{
    return hwnd == g_hMainWnd;
}

static VOID
Preview_UpdateZoom(PPREVIEW_DATA pData, UINT NewZoom, BOOL bEnableBestFit, BOOL bEnableRealSize)
{
    BOOL bEnableZoomIn, bEnableZoomOut;
    HWND hToolBar = pData->m_hwndToolBar;

    /* If zoom has not been changed, ignore it */
    if (pData->m_nZoomPercents == NewZoom)
        return;

    pData->m_nZoomPercents = NewZoom;

    /* Check if a zoom button of the toolbar must be grayed */
    bEnableZoomIn = (NewZoom < MAX_ZOOM);
    bEnableZoomOut = (NewZoom > MIN_ZOOM);

    /* Update toolbar buttons */
    SendMessageW(hToolBar, TB_ENABLEBUTTON, IDC_ZOOM_OUT, bEnableZoomOut);
    SendMessageW(hToolBar, TB_ENABLEBUTTON, IDC_ZOOM_IN,  bEnableZoomIn);
    SendMessageW(hToolBar, TB_ENABLEBUTTON, IDC_BEST_FIT, bEnableBestFit);
    SendMessageW(hToolBar, TB_ENABLEBUTTON, IDC_REAL_SIZE, bEnableRealSize);

    /* Redraw the display window */
    InvalidateRect(pData->m_hwndZoom, NULL, FALSE);
}

static VOID
Preview_ZoomInOrOut(PPREVIEW_DATA pData, BOOL bZoomIn)
{
    UINT i, NewZoom;

    if (g_pImage == NULL)
        return;

    if (bZoomIn)    /* zoom in */
    {
        /* find next step */
        for (i = 0; i < _countof(s_ZoomSteps); ++i)
        {
            if (pData->m_nZoomPercents < s_ZoomSteps[i])
                break;
        }
        NewZoom = ((i >= _countof(s_ZoomSteps)) ? MAX_ZOOM : s_ZoomSteps[i]);
    }
    else            /* zoom out */
    {
        /* find previous step */
        for (i = _countof(s_ZoomSteps); i > 0; )
        {
            --i;
            if (s_ZoomSteps[i] < pData->m_nZoomPercents)
                break;
        }
        NewZoom = ((i < 0) ? MIN_ZOOM : s_ZoomSteps[i]);
    }

    /* Update toolbar and refresh screen */
    Preview_UpdateZoom(pData, NewZoom, TRUE, TRUE);
}

static VOID
Preview_ResetZoom(PPREVIEW_DATA pData)
{
    RECT Rect;
    UINT ImageWidth, ImageHeight, NewZoom;

    if (g_pImage == NULL)
        return;

    /* get disp window size and image size */
    GetClientRect(pData->m_hwndZoom, &Rect);
    GdipGetImageWidth(g_pImage, &ImageWidth);
    GdipGetImageHeight(g_pImage, &ImageHeight);

    /* compare two aspect rates. same as
       (ImageHeight / ImageWidth < Rect.bottom / Rect.right) in real */
    if (ImageHeight * Rect.right < Rect.bottom * ImageWidth)
    {
        if (Rect.right < ImageWidth)
        {
            /* it's large, shrink it */
            NewZoom = (Rect.right * 100) / ImageWidth;
        }
        else
        {
            /* it's small. show as original size */
            NewZoom = 100;
        }
    }
    else
    {
        if (Rect.bottom < ImageHeight)
        {
            /* it's large, shrink it */
            NewZoom = (Rect.bottom * 100) / ImageHeight;
        }
        else
        {
            /* it's small. show as original size */
            NewZoom = 100;
        }
    }

    Preview_UpdateZoom(pData, NewZoom, FALSE, TRUE);
}

static VOID
Preview_UpdateTitle(PPREVIEW_DATA pData, LPCWSTR FileName)
{
    WCHAR szText[MAX_PATH + 100];
    LPWSTR pchFileTitle;

    LoadStringW(g_hInstance, IDS_APPTITLE, szText, _countof(szText));

    pchFileTitle = PathFindFileNameW(FileName);
    if (pchFileTitle && *pchFileTitle)
    {
        StringCchCatW(szText, _countof(szText), L" - ");
        StringCchCatW(szText, _countof(szText), pchFileTitle);
    }

    SetWindowTextW(pData->m_hwnd, szText);
}

static VOID
Preview_pLoadImage(PPREVIEW_DATA pData, LPCWSTR szOpenFileName)
{
    Anime_FreeInfo(&pData->m_Anime);

    if (g_pImage)
    {
        GdipDisposeImage(g_pImage);
        g_pImage = NULL;
    }

    /* check file presence */
    if (!szOpenFileName || GetFileAttributesW(szOpenFileName) == 0xFFFFFFFF)
    {
        DPRINT1("File %s not found!\n", szOpenFileName);
        Preview_UpdateTitle(pData, NULL);
        return;
    }

    /* load now */
    GdipLoadImageFromFile(szOpenFileName, &g_pImage);
    if (!g_pImage)
    {
        DPRINT1("GdipLoadImageFromFile() failed\n");
        Preview_UpdateTitle(pData, NULL);
        return;
    }

    Anime_LoadInfo(&pData->m_Anime);

    if (szOpenFileName && szOpenFileName[0])
        SHAddToRecentDocs(SHARD_PATHW, szOpenFileName);

    /* Reset zoom and redraw display */
    Preview_ResetZoom(pData);

    Preview_UpdateTitle(pData, szOpenFileName);
}

static VOID
Preview_pLoadImageFromNode(PPREVIEW_DATA pData, SHIMGVW_FILENODE *pNode)
{
    Preview_pLoadImage(pData, (pNode ? pNode->FileName : NULL));
}

static VOID
Preview_pSaveImageAs(PPREVIEW_DATA pData)
{
    OPENFILENAMEW sfn;
    ImageCodecInfo *codecInfo;
    WCHAR szSaveFileName[MAX_PATH];
    WCHAR *szFilterMask;
    GUID rawFormat;
    UINT num, size, j;
    size_t sizeRemain;
    WCHAR *c;
    HWND hwnd = pData->m_hwnd;

    if (g_pImage == NULL)
        return;

    GdipGetImageEncodersSize(&num, &size);
    codecInfo = QuickAlloc(size, FALSE);
    if (!codecInfo)
    {
        DPRINT1("QuickAlloc() failed in pSaveImageAs()\n");
        return;
    }

    GdipGetImageEncoders(num, size, codecInfo);
    GdipGetImageRawFormat(g_pImage, &rawFormat);

    sizeRemain = 0;
    for (j = 0; j < num; ++j)
    {
        // Every pair needs space for the Description, twice the Extensions, 1 char for the space, 2 for the braces and 2 for the NULL terminators.
        sizeRemain = sizeRemain + (((wcslen(codecInfo[j].FormatDescription) + (wcslen(codecInfo[j].FilenameExtension) * 2) + 5) * sizeof(WCHAR)));
    }

    /* Add two more chars for the last terminator */
    sizeRemain += (sizeof(WCHAR) * 2);

    szFilterMask = QuickAlloc(sizeRemain, FALSE);
    if (!szFilterMask)
    {
        DPRINT1("cannot allocate memory for filter mask in pSaveImageAs()");
        QuickFree(codecInfo);
        return;
    }

    ZeroMemory(szSaveFileName, sizeof(szSaveFileName));
    ZeroMemory(szFilterMask, sizeRemain);
    ZeroMemory(&sfn, sizeof(sfn));
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner   = hwnd;
    sfn.lpstrFile   = szSaveFileName;
    sfn.lpstrFilter = szFilterMask;
    sfn.nMaxFile    = _countof(szSaveFileName);
    sfn.Flags       = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    sfn.lpstrDefExt = L"png";

    c = szFilterMask;

    for (j = 0; j < num; ++j)
    {
        StringCbPrintfExW(c, sizeRemain, &c, &sizeRemain, 0, L"%ls (%ls)", codecInfo[j].FormatDescription, codecInfo[j].FilenameExtension);

        /* Skip the NULL character */
        c++;
        sizeRemain -= sizeof(*c);

        StringCbPrintfExW(c, sizeRemain, &c, &sizeRemain, 0, L"%ls", codecInfo[j].FilenameExtension);

        /* Skip the NULL character */
        c++;
        sizeRemain -= sizeof(*c);

        if (IsEqualGUID(&rawFormat, &codecInfo[j].FormatID) != FALSE)
        {
            sfn.nFilterIndex = j + 1;
        }
    }

    if (GetSaveFileNameW(&sfn))
    {
        Anime_Pause(&pData->m_Anime);

        if (GdipSaveImageToFile(g_pImage, szSaveFileName, &codecInfo[sfn.nFilterIndex - 1].Clsid, NULL) != Ok)
        {
            DPRINT1("GdipSaveImageToFile() failed\n");
        }

        Anime_Start(&pData->m_Anime, 0);
    }

    QuickFree(szFilterMask);
    QuickFree(codecInfo);
}

static VOID
Preview_pPrintImage(PPREVIEW_DATA pData)
{
    /* FIXME */
}

static VOID
Preview_UpdateUI(PPREVIEW_DATA pData)
{
    BOOL bEnable = (g_pImage != NULL);
    SendMessageW(pData->m_hwndToolBar, TB_ENABLEBUTTON, IDC_SAVEAS, bEnable);
    SendMessageW(pData->m_hwndToolBar, TB_ENABLEBUTTON, IDC_PRINT, bEnable);

    if (!Preview_IsMainWnd(pData->m_hwnd))
        Preview_ResetZoom(pData);

    InvalidateRect(pData->m_hwndZoom, NULL, FALSE);
}

static SHIMGVW_FILENODE*
pBuildFileList(LPCWSTR szFirstFile)
{
    HANDLE hFindHandle;
    WCHAR *extension;
    WCHAR szSearchPath[MAX_PATH];
    WCHAR szSearchMask[MAX_PATH];
    WCHAR szFileTypes[MAX_PATH];
    WIN32_FIND_DATAW findData;
    SHIMGVW_FILENODE *currentNode = NULL;
    SHIMGVW_FILENODE *root = NULL;
    SHIMGVW_FILENODE *conductor = NULL;
    ImageCodecInfo *codecInfo;
    UINT num;
    UINT size;
    UINT j;

    StringCbCopyW(szSearchPath, sizeof(szSearchPath), szFirstFile);
    PathRemoveFileSpecW(szSearchPath);

    GdipGetImageDecodersSize(&num, &size);
    codecInfo = QuickAlloc(size, FALSE);
    if (!codecInfo)
    {
        DPRINT1("QuickAlloc() failed in pLoadFileList()\n");
        return NULL;
    }

    GdipGetImageDecoders(num, size, codecInfo);

    root = QuickAlloc(sizeof(SHIMGVW_FILENODE), FALSE);
    if (!root)
    {
        DPRINT1("QuickAlloc() failed in pLoadFileList()\n");
        QuickFree(codecInfo);
        return NULL;
    }

    conductor = root;

    for (j = 0; j < num; ++j)
    {
        StringCbCopyW(szFileTypes, sizeof(szFileTypes), codecInfo[j].FilenameExtension);

        extension = wcstok(szFileTypes, L";");
        while (extension != NULL)
        {
            PathCombineW(szSearchMask, szSearchPath, extension);

            hFindHandle = FindFirstFileW(szSearchMask, &findData);
            if (hFindHandle != INVALID_HANDLE_VALUE)
            {
                do
                {
                    PathCombineW(conductor->FileName, szSearchPath, findData.cFileName);

                    // compare the name of the requested file with the one currently found.
                    // if the name matches, the current node is returned by the function.
                    if (_wcsicmp(szFirstFile, conductor->FileName) == 0)
                    {
                        currentNode = conductor;
                    }

                    conductor->Next = QuickAlloc(sizeof(SHIMGVW_FILENODE), FALSE);

                    // if QuickAlloc fails, make circular what we have and return it
                    if (!conductor->Next)
                    {
                        DPRINT1("QuickAlloc() failed in pLoadFileList()\n");

                        conductor->Next = root;
                        root->Prev = conductor;

                        FindClose(hFindHandle);
                        QuickFree(codecInfo);
                        return conductor;
                    }

                    conductor->Next->Prev = conductor;
                    conductor = conductor->Next;
                }
                while (FindNextFileW(hFindHandle, &findData) != 0);

                FindClose(hFindHandle);
            }

            extension = wcstok(NULL, L";");
        }
    }

    // we now have a node too much in the list. In case the requested file was not found,
    // we use this node to store the name of it, otherwise we free it.
    if (currentNode == NULL)
    {
        StringCchCopyW(conductor->FileName, MAX_PATH, szFirstFile);
        currentNode = conductor;
    }
    else
    {
        conductor = conductor->Prev;
        QuickFree(conductor->Next);
    }

    // link the last node with the first one to make the list circular
    conductor->Next = root;
    root->Prev = conductor;
    conductor = currentNode;

    QuickFree(codecInfo);

    return conductor;
}

static VOID
pFreeFileList(SHIMGVW_FILENODE *root)
{
    SHIMGVW_FILENODE *conductor;

    if (!root)
        return;

    root->Prev->Next = NULL;
    root->Prev = NULL;

    while (root)
    {
        conductor = root;
        root = conductor->Next;
        QuickFree(conductor);
    }
}

static HBRUSH CreateCheckerBoardBrush(HDC hdc)
{
    static const CHAR pattern[] =
        "\x28\x00\x00\x00\x10\x00\x00\x00\x10\x00\x00\x00\x01\x00\x04\x00\x00\x00"
        "\x00\x00\x80\x00\x00\x00\x23\x2E\x00\x00\x23\x2E\x00\x00\x10\x00\x00\x00"
        "\x00\x00\x00\x00\x99\x99\x99\x00\xCC\xCC\xCC\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x11\x11\x11\x11"
        "\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00"
        "\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00"
        "\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11"
        "\x11\x11\x00\x00\x00\x00\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00"
        "\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11"
        "\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11"
        "\x00\x00\x00\x00\x11\x11\x11\x11\x00\x00\x00\x00\x11\x11\x11\x11";

    return CreateDIBPatternBrushPt(pattern, DIB_RGB_COLORS);
}

static VOID
ZoomWnd_OnPaint(PPREVIEW_DATA pData, HWND hwnd)
{
    GpGraphics *graphics;
    INT ZoomedWidth, ZoomedHeight, x, y;
    PAINTSTRUCT ps;
    RECT rect, margin;
    HDC hdc;
    HBRUSH hBrush;
    HPEN hPen;
    HGDIOBJ hbrOld, hPenOld, hFontOld;
    UINT uFlags;

    hdc = BeginPaint(hwnd, &ps);
    if (!hdc)
    {
        DPRINT1("BeginPaint() failed\n");
        return;
    }

    GdipCreateFromHDC(hdc, &graphics);
    if (!graphics)
    {
        DPRINT1("GdipCreateFromHDC() failed\n");
        return;
    }

    GetClientRect(hwnd, &rect);

    if (Preview_IsMainWnd(pData->m_hwnd))
    {
        hPen = (HPEN)GetStockObject(BLACK_PEN);
        hBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
        SetTextColor(hdc, RGB(0, 0, 0)); /* Black text */
    }
    else
    {
        hPen = (HPEN)GetStockObject(WHITE_PEN);
        hBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
        SetTextColor(hdc, RGB(255, 255, 255)); /* White text */
    }

    if (g_pImage == NULL)
    {
        WCHAR szText[128];
        LoadStringW(g_hInstance, IDS_NOPREVIEW, szText, _countof(szText));

        FillRect(hdc, &rect, hBrush);

        hFontOld = SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, szText, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER |
                                          DT_NOPREFIX);
        SelectObject(hdc, hFontOld);
    }
    else
    {
        UINT ImageWidth, ImageHeight;

        GdipGetImageWidth(g_pImage, &ImageWidth);
        GdipGetImageHeight(g_pImage, &ImageHeight);

        ZoomedWidth = (ImageWidth * pData->m_nZoomPercents) / 100;
        ZoomedHeight = (ImageHeight * pData->m_nZoomPercents) / 100;

        x = (rect.right - ZoomedWidth) / 2;
        y = (rect.bottom - ZoomedHeight) / 2;

        // Fill top part
        margin = rect;
        margin.bottom = y - 1;
        FillRect(hdc, &margin, hBrush);
        // Fill bottom part
        margin.top = y + ZoomedHeight + 1;
        margin.bottom = rect.bottom;
        FillRect(hdc, &margin, hBrush);
        // Fill left part
        margin.top = y - 1;
        margin.bottom = y + ZoomedHeight + 1;
        margin.right = x - 1;
        FillRect(hdc, &margin, hBrush);
        // Fill right part
        margin.left = x + ZoomedWidth + 1;
        margin.right = rect.right;
        FillRect(hdc, &margin, hBrush);

        DPRINT("x = %d, y = %d, ImageWidth = %u, ImageHeight = %u\n");
        DPRINT("rect.right = %ld, rect.bottom = %ld\n", rect.right, rect.bottom);
        DPRINT("m_nZoomPercents = %d, ZoomedWidth = %d, ZoomedHeight = %d\n",
               pData->m_nZoomPercents, ZoomedWidth, ZoomedWidth);

        if (pData->m_nZoomPercents % 100 == 0)
        {
            GdipSetInterpolationMode(graphics, InterpolationModeNearestNeighbor);
            GdipSetSmoothingMode(graphics, SmoothingModeNone);
        }
        else
        {
            GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBilinear);
            GdipSetSmoothingMode(graphics, SmoothingModeHighQuality);
        }

        uFlags = 0;
        GdipGetImageFlags(g_pImage, &uFlags);

        hPenOld = SelectObject(hdc, hPen);
        if (uFlags & (ImageFlagsHasAlpha | ImageFlagsHasTranslucent))
        {
            HBRUSH hbr = CreateCheckerBoardBrush(hdc);
            hbrOld = SelectObject(hdc, hbr);
            Rectangle(hdc, x - 1, y - 1, x + ZoomedWidth + 1, y + ZoomedHeight + 1);
            SelectObject(hdc, hbrOld);
            DeleteObject(hbr);
        }
        else
        {
            hbrOld = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, x - 1, y - 1, x + ZoomedWidth + 1, y + ZoomedHeight + 1);
            SelectObject(hdc, hbrOld);
        }
        SelectObject(hdc, hPenOld);

        GdipDrawImageRectI(graphics, g_pImage, x, y, ZoomedWidth, ZoomedHeight);
    }
    GdipDeleteGraphics(graphics);
    EndPaint(hwnd, &ps);
}

static VOID
ImageView_ResetSettings(VOID)
{
    g_Settings.Maximized = FALSE;
    g_Settings.X         = CW_USEDEFAULT;
    g_Settings.Y         = CW_USEDEFAULT;
    g_Settings.Width     = 520;
    g_Settings.Height    = 400;
}

static BOOL
ImageView_LoadSettings(VOID)
{
    HKEY hKey;
    DWORD dwSize;
    LSTATUS nError;

    nError = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ReactOS\\shimgvw", 0, KEY_READ, &hKey);
    if (nError != ERROR_SUCCESS)
        return FALSE;

    dwSize = sizeof(g_Settings);
    nError = RegQueryValueExW(hKey, L"Settings", NULL, NULL, (LPBYTE)&g_Settings, &dwSize);
    RegCloseKey(hKey);

    return ((nError == ERROR_SUCCESS) && (dwSize == sizeof(g_Settings)));
}

static VOID
ImageView_SaveSettings(VOID)
{
    HKEY hKey;
    LSTATUS nError;

    nError = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ReactOS\\shimgvw",
                             0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (nError != ERROR_SUCCESS)
        return;

    RegSetValueExW(hKey, L"Settings", 0, REG_BINARY, (LPBYTE)&g_Settings, sizeof(g_Settings));
    RegCloseKey(hKey);
}

static BOOL
Preview_CreateToolBar(PPREVIEW_DATA pData)
{
    HWND hwndToolBar;
    HIMAGELIST hImageList, hOldImageList;
    DWORD style = WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS;
    DWORD exstyle = 0;

    if (!Preview_IsMainWnd(pData->m_hwnd))
        return TRUE; /* FIXME */

    style |= CCS_BOTTOM;
    hwndToolBar = CreateWindowExW(exstyle, TOOLBARCLASSNAMEW, NULL, style,
                                  0, 0, 0, 0, pData->m_hwnd, NULL, g_hInstance, NULL);
    if (!hwndToolBar)
        return FALSE;

    pData->m_hwndToolBar = hwndToolBar;

    SendMessageW(hwndToolBar, TB_BUTTONSTRUCTSIZE, sizeof(s_Buttons[0]), 0);
    SendMessageW(hwndToolBar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_HIDECLIPPEDBUTTONS);

    hImageList = ImageList_Create(TB_IMAGE_WIDTH, TB_IMAGE_HEIGHT, ILC_MASK | ILC_COLOR24, 1, 1);
    if (hImageList == NULL)
        return FALSE;

    for (UINT n = 0; n < _countof(s_ButtonConfig); n++)
    {
        HBITMAP hBitmap = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(s_ButtonConfig[n].idb));
        ImageList_AddMasked(hImageList, hBitmap, RGB(255, 255, 255));
        DeleteObject(hBitmap);
    }

    hOldImageList = (HIMAGELIST)SendMessageW(hwndToolBar, TB_SETIMAGELIST, 0, (LPARAM)hImageList);
    ImageList_Destroy(hOldImageList);

    SendMessageW(hwndToolBar, TB_ADDBUTTONS, _countof(s_Buttons), (LPARAM)s_Buttons);

    return TRUE;
}

LRESULT CALLBACK
ZoomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    switch (uMsg)
    {
        case WM_PAINT:
        {
            ZoomWnd_OnPaint(pData, hwnd);
            break;
        }
        case WM_TIMER:
        {
            if (Anime_OnTimer(&pData->m_Anime, wParam))
                InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        default:
        {
            return CallWindowProcW(pData->m_fnPrevProc, hwnd, uMsg, wParam, lParam);
        }
    }
    return 0;
}

static BOOL
Preview_OnCreate(HWND hwnd, LPCREATESTRUCT pCS)
{
    HWND hwndZoom;
    PPREVIEW_DATA pData = QuickAlloc(sizeof(PREVIEW_DATA), TRUE);
    pData->m_hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pData);

    if (g_hMainWnd == NULL)
        g_hMainWnd = hwnd;
    else if (g_hwndFullscreen == NULL)
        g_hwndFullscreen = hwnd;
    else
        return FALSE;

    DragAcceptFiles(hwnd, TRUE);

    hwndZoom = CreateWindowExW(WS_EX_CLIENTEDGE, WC_STATIC, NULL, WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, hwnd, NULL, g_hInstance, NULL);
    if (!hwndZoom)
    {
        QuickFree(pData);
        return FALSE;
    }

    pData->m_hwndZoom = hwndZoom;
    SetWindowLongPtrW(hwndZoom, GWLP_USERDATA, (LONG_PTR)pData);

    SetClassLongPtr(pData->m_hwndZoom, GCL_STYLE, CS_HREDRAW | CS_VREDRAW);
    pData->m_fnPrevProc = (WNDPROC)SetWindowLongPtrW(pData->m_hwndZoom,
                                                    GWLP_WNDPROC, (LPARAM)ZoomWndProc);

    if (!Preview_CreateToolBar(pData))
    {
        QuickFree(pData);
        return FALSE;
    }

    Anime_SetTimerWnd(&pData->m_Anime, pData->m_hwndZoom);

    if (pCS && pCS->lpCreateParams)
    {
        LPCWSTR pszFileName = (LPCWSTR)pCS->lpCreateParams;
        WCHAR szFile[MAX_PATH];

        /* Make sure the path has no quotes on it */
        StringCchCopyW(szFile, _countof(szFile), pszFileName);
        PathUnquoteSpacesW(szFile);

        g_pCurrentFile = pBuildFileList(szFile);
        Preview_pLoadImageFromNode(pData, g_pCurrentFile);
        Preview_UpdateUI(pData);
    }

    return TRUE;
}

static VOID
Preview_OnMouseWheel(HWND hwnd, INT x, INT y, INT zDelta, UINT fwKeys)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    if (zDelta != 0)
    {
        if (GetKeyState(VK_CONTROL) < 0)
            Preview_ZoomInOrOut(pData, zDelta > 0);
    }
}

static VOID
Preview_OnMoveSize(HWND hwnd)
{
    WINDOWPLACEMENT wp;
    RECT *prc;

    if (IsIconic(hwnd) || !Preview_IsMainWnd(hwnd))
        return;

    wp.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &wp);

    prc = &wp.rcNormalPosition;
    g_Settings.X = prc->left;
    g_Settings.Y = prc->top;
    g_Settings.Width = prc->right - prc->left;
    g_Settings.Height = prc->bottom - prc->top;
    g_Settings.Maximized = IsZoomed(hwnd);
}

static VOID
Preview_OnSize(HWND hwnd)
{
    RECT rc, rcClient;
    PPREVIEW_DATA pData = Preview_GetData(hwnd);
    HWND hToolBar = pData->m_hwndToolBar;
    INT cx, cy;

    /* We want 32-bit values. Don't use WM_SIZE lParam */
    GetClientRect(hwnd, &rcClient);
    cx = rcClient.right;
    cy = rcClient.bottom;

    if (Preview_IsMainWnd(pData->m_hwnd))
    {
        SendMessageW(hToolBar, TB_AUTOSIZE, 0, 0);
        GetWindowRect(hToolBar, &rc);

        MoveWindow(pData->m_hwndZoom, 0, 0, cx, cy - (rc.bottom - rc.top), TRUE);

        if (!IsIconic(hwnd)) /* Is it not minimized? */
            Preview_ResetZoom(pData);

        Preview_OnMoveSize(hwnd);
    }
    else
    {
        MoveWindow(pData->m_hwndZoom, 0, 0, cx, cy, TRUE);
    }
}

static VOID
Preview_EndSlideShow(HWND hwnd)
{
    if (Preview_IsMainWnd(hwnd))
        return;

    KillTimer(hwnd, SLIDESHOW_TIMER_ID);
    ShowWindow(hwnd, SW_HIDE);
    ShowWindow(g_hMainWnd, SW_SHOWNORMAL);
    Preview_ResetZoom(Preview_GetData(g_hMainWnd));
}

static VOID
Preview_Delete(PPREVIEW_DATA pData)
{
    WCHAR szCurFile[MAX_PATH + 1], szNextFile[MAX_PATH];
    HWND hwnd = pData->m_hwnd;
    SHFILEOPSTRUCTW FileOp = { hwnd, FO_DELETE };

    if (!g_pCurrentFile)
        return;

    /* FileOp.pFrom must be double-null-terminated */
    GetFullPathNameW(g_pCurrentFile->FileName, _countof(szCurFile) - 1, szCurFile, NULL);
    szCurFile[_countof(szCurFile) - 2] = UNICODE_NULL; /* Avoid buffer overrun */
    szCurFile[lstrlenW(szCurFile) + 1] = UNICODE_NULL;

    GetFullPathNameW(g_pCurrentFile->Next->FileName, _countof(szNextFile), szNextFile, NULL);
    szNextFile[_countof(szNextFile) - 1] = UNICODE_NULL; /* Avoid buffer overrun */

    /* FIXME: Our GdipLoadImageFromFile locks the image file */
    if (g_pImage)
    {
        GdipDisposeImage(g_pImage);
        g_pImage = NULL;
    }

    /* Confirm file deletion and delete if allowed */
    FileOp.pFrom = szCurFile;
    FileOp.fFlags = FOF_ALLOWUNDO;
    if (SHFileOperationW(&FileOp) != 0)
    {
        DPRINT("Preview_Delete: SHFileOperationW() failed or canceled\n");

        Preview_pLoadImage(pData, szCurFile);
        return;
    }

    /* Reload the file list and go next file */
    pFreeFileList(g_pCurrentFile);
    g_pCurrentFile = pBuildFileList(szNextFile);
    Preview_pLoadImageFromNode(pData, g_pCurrentFile);
}

static VOID
Preview_Edit(HWND hwnd)
{
    WCHAR szPathName[MAX_PATH];
    SHELLEXECUTEINFOW sei;

    if (!g_pCurrentFile)
        return;

    /* Avoid file locking */
    /* FIXME: Our GdipLoadImageFromFile locks the image file */
    if (g_pImage)
    {
        GdipDisposeImage(g_pImage);
        g_pImage = NULL;
    }

    GetFullPathNameW(g_pCurrentFile->FileName, _countof(szPathName), szPathName, NULL);
    szPathName[_countof(szPathName) - 1] = UNICODE_NULL; /* Avoid buffer overrun */

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"edit";
    sei.lpFile = szPathName;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
    {
        DPRINT1("Preview_Edit: ShellExecuteExW() failed with code %ld\n", GetLastError());
    }

    // Destroy the window to quit the application
    DestroyWindow(hwnd);
}

static VOID
Preview_StartSlideShow(PPREVIEW_DATA pData)
{
    HWND hwnd = g_hwndFullscreen;
    DWORD style = WS_POPUP | WS_CLIPSIBLINGS, exstyle = WS_EX_TOPMOST;

    ShowWindow(pData->m_hwnd, SW_HIDE);

    if (!IsWindow(hwnd))
    {
        WCHAR szTitle[256];
        LoadStringW(g_hInstance, IDS_APPTITLE, szTitle, _countof(szTitle));
        hwnd = CreateWindowExW(exstyle, WC_PREVIEW, szTitle, style,
                               0, 0, 0, 0, NULL, NULL, g_hInstance, NULL);
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    SetTimer(hwnd, SLIDESHOW_TIMER_ID, SLIDESHOW_TIMER_INTERVAL, NULL);
}

static VOID
Preview_GoNextPic(PPREVIEW_DATA pData, BOOL bNext)
{
    HWND hwnd = pData->m_hwnd;
    if (!Preview_IsMainWnd(hwnd))
    {
        KillTimer(hwnd, SLIDESHOW_TIMER_ID);
        SetTimer(hwnd, SLIDESHOW_TIMER_ID, SLIDESHOW_TIMER_INTERVAL, NULL);
    }

    if (g_pCurrentFile)
    {
        if (bNext)
            g_pCurrentFile = g_pCurrentFile->Next;
        else
            g_pCurrentFile = g_pCurrentFile->Prev;
        Preview_pLoadImageFromNode(pData, g_pCurrentFile);
        Preview_UpdateUI(pData);
    }
}

static VOID
Preview_OnCommand(HWND hwnd, UINT nCommandID)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);

    switch (nCommandID)
    {
        case IDC_PREV_PIC:
            Preview_GoNextPic(pData, FALSE);
            break;

        case IDC_NEXT_PIC:
            Preview_GoNextPic(pData, TRUE);
            break;

        case IDC_BEST_FIT:
            Preview_ResetZoom(pData);
            break;

        case IDC_REAL_SIZE:
            Preview_UpdateZoom(pData, 100, TRUE, FALSE);
            break;

        case IDC_SLIDE_SHOW:
            if (Preview_IsMainWnd(hwnd))
                Preview_StartSlideShow(pData);
            else
                Preview_EndSlideShow(hwnd);
            break;

        case IDC_ZOOM_IN:
            Preview_ZoomInOrOut(pData, TRUE);
            break;

        case IDC_ZOOM_OUT:
            Preview_ZoomInOrOut(pData, FALSE);
            break;

        case IDC_ENDSLIDESHOW:
            Preview_EndSlideShow(hwnd);
            break;

        default:
            break;
    }

    if (!Preview_IsMainWnd(hwnd))
        return;

    // The following commands are for main window only:
    switch (nCommandID)
    {
        case IDC_SAVEAS:
            Preview_pSaveImageAs(pData);
            break;

        case IDC_PRINT:
            Preview_pPrintImage(pData);
            break;

        case IDC_ROT_CLOCKW:
            if (g_pImage)
            {
                GdipImageRotateFlip(g_pImage, Rotate270FlipNone);
                Preview_UpdateUI(pData);
            }
            break;

        case IDC_ROT_COUNCW:
            if (g_pImage)
            {
                GdipImageRotateFlip(g_pImage, Rotate90FlipNone);
                Preview_UpdateUI(pData);
            }
            break;

        case IDC_DELETE:
            Preview_Delete(pData);
            Preview_UpdateUI(pData);
            break;

        case IDC_MODIFY:
            Preview_Edit(hwnd);
            Preview_UpdateUI(pData);
            break;

        default:
            break;
    }
}

static LRESULT
Preview_OnNotify(HWND hwnd, LPNMHDR pnmhdr)
{
    switch (pnmhdr->code)
    {
        case TTN_GETDISPINFOW:
        {
            LPTOOLTIPTEXTW lpttt = (LPTOOLTIPTEXTW)pnmhdr;
            lpttt->hinst = g_hInstance;
            lpttt->lpszText = MAKEINTRESOURCEW(s_ButtonConfig[lpttt->hdr.idFrom - IDC_TOOL_BASE].ids);
            break;
        }
    }
    return 0;
}

static VOID
Preview_OnDestroy(HWND hwnd)
{
    PPREVIEW_DATA pData = Preview_GetData(hwnd);

    KillTimer(hwnd, SLIDESHOW_TIMER_ID);

    pFreeFileList(g_pCurrentFile);
    g_pCurrentFile = NULL;

    if (g_pImage)
    {
        GdipDisposeImage(g_pImage);
        g_pImage = NULL;
    }

    Anime_FreeInfo(&pData->m_Anime);

    SetWindowLongPtrW(pData->m_hwndZoom, GWLP_WNDPROC, (LPARAM)pData->m_fnPrevProc);
    SetWindowLongPtrW(pData->m_hwndZoom, GWLP_USERDATA, 0);
    DestroyWindow(pData->m_hwndZoom);
    pData->m_hwndZoom = NULL;

    DestroyWindow(pData->m_hwndToolBar);
    pData->m_hwndToolBar = NULL;

    QuickFree(pData);

    PostQuitMessage(0);
}

static VOID
Preview_OnDropFiles(HWND hwnd, HDROP hDrop)
{
    WCHAR szFile[MAX_PATH];
    PPREVIEW_DATA pData = Preview_GetData(hwnd);

    DragQueryFileW(hDrop, 0, szFile, _countof(szFile));

    pFreeFileList(g_pCurrentFile);
    g_pCurrentFile = pBuildFileList(szFile);
    Preview_pLoadImageFromNode(pData, g_pCurrentFile);

    DragFinish(hDrop);
}

static VOID
Preview_OnButtonDown(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (!Preview_IsMainWnd(hwnd))
        Preview_EndSlideShow(hwnd);
}

LRESULT CALLBACK
PreviewWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            if (!Preview_OnCreate(hwnd, (LPCREATESTRUCT)lParam))
                return -1;
            break;
        }
        case WM_COMMAND:
        {
            Preview_OnCommand(hwnd, LOWORD(wParam));
            break;
        }
        case WM_MOUSEWHEEL:
        {
            Preview_OnMouseWheel(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                                 (SHORT)HIWORD(wParam), (UINT)LOWORD(wParam));
            break;
        }
        case WM_NOTIFY:
        {
            return Preview_OnNotify(hwnd, (LPNMHDR)lParam);
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO *pMMI = (MINMAXINFO*)lParam;
            pMMI->ptMinTrackSize.x = 350;
            pMMI->ptMinTrackSize.y = 290;
            break;
        }
        case WM_MOVE:
        {
            Preview_OnMoveSize(hwnd);
            break;
        }
        case WM_SIZE:
        {
            Preview_OnSize(hwnd);
            break;
        }
        case WM_DROPFILES:
        {
            Preview_OnDropFiles(hwnd, (HDROP)wParam);
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
        {
            Preview_OnButtonDown(hwnd, uMsg, wParam, lParam);
            break;
        }
        case WM_DESTROY:
        {
            Preview_OnDestroy(hwnd);
            break;
        }
        case WM_TIMER:
        {
            if (wParam == SLIDESHOW_TIMER_ID)
            {
                PPREVIEW_DATA pData = Preview_GetData(hwnd);
                Preview_GoNextPic(pData, TRUE);
            }
            break;
        }
        default:
        {
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
    }

    return 0;
}

LONG
ImageView_Main(HWND hwnd, LPCWSTR szFileName)
{
    struct GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    WNDCLASSW WndClass;
    WCHAR szTitle[256];
    HWND hMainWnd;
    MSG msg;
    HACCEL hAccel;
    HRESULT hrCoInit;
    INITCOMMONCONTROLSEX Icc = { .dwSize = sizeof(Icc), .dwICC = ICC_WIN95_CLASSES };

    InitCommonControlsEx(&Icc);

    /* Initialize COM */
    hrCoInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCoInit))
        DPRINT1("Warning, CoInitializeEx failed with code=%08X\n", (int)hrCoInit);

    if (!ImageView_LoadSettings())
        ImageView_ResetSettings();

    /* Initialize GDI+ */
    ZeroMemory(&gdiplusStartupInput, sizeof(gdiplusStartupInput));
    gdiplusStartupInput.GdiplusVersion = 1;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    /* Register window classes */
    ZeroMemory(&WndClass, sizeof(WndClass));
    WndClass.lpszClassName  = WC_PREVIEW;
    WndClass.lpfnWndProc    = PreviewWndProc;
    WndClass.hInstance      = g_hInstance;
    WndClass.style          = CS_HREDRAW | CS_VREDRAW;
    WndClass.hIcon          = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    WndClass.hCursor        = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    WndClass.hbrBackground  = NULL;   /* less flicker */
    if (!RegisterClassW(&WndClass))
        return -1;

    /* Create the main window */
    LoadStringW(g_hInstance, IDS_APPTITLE, szTitle, _countof(szTitle));
    hMainWnd = CreateWindowExW(WS_EX_WINDOWEDGE, WC_PREVIEW, szTitle,
                               WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS,
                               g_Settings.X, g_Settings.Y, g_Settings.Width, g_Settings.Height,
                               NULL, NULL, g_hInstance, (LPVOID)szFileName);

    /* Create accelerator table for keystrokes */
    hAccel = LoadAcceleratorsW(g_hInstance, MAKEINTRESOURCEW(IDR_ACCELERATOR));

    /* Show the main window now */
    if (g_Settings.Maximized)
        ShowWindow(hMainWnd, SW_SHOWMAXIMIZED);
    else
        ShowWindow(hMainWnd, SW_SHOWNORMAL);

    UpdateWindow(hMainWnd);

    /* Message Loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (g_hwndFullscreen && TranslateAcceleratorW(g_hwndFullscreen, hAccel, &msg))
            continue;
        if (TranslateAcceleratorW(hMainWnd, hAccel, &msg))
            continue;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Destroy accelerator table */
    DestroyAcceleratorTable(hAccel);

    ImageView_SaveSettings();

    GdiplusShutdown(gdiplusToken);

    /* Release COM resources */
    if (SUCCEEDED(hrCoInit))
        CoUninitialize();

    return 0;
}

VOID WINAPI
ImageView_FullscreenW(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    ImageView_Main(hwnd, path);
}

VOID WINAPI
ImageView_Fullscreen(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    ImageView_Main(hwnd, path);
}

VOID WINAPI
ImageView_FullscreenA(HWND hwnd, HINSTANCE hInst, LPCSTR path, int nShow)
{
    WCHAR szFile[MAX_PATH];

    if (MultiByteToWideChar(CP_ACP, 0, path, -1, szFile, _countof(szFile)))
    {
        ImageView_Main(hwnd, szFile);
    }
}

VOID WINAPI
ImageView_PrintTo(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    DPRINT("ImageView_PrintTo() not implemented\n");
}

VOID WINAPI
ImageView_PrintToA(HWND hwnd, HINSTANCE hInst, LPCSTR path, int nShow)
{
    DPRINT("ImageView_PrintToA() not implemented\n");
}

VOID WINAPI
ImageView_PrintToW(HWND hwnd, HINSTANCE hInst, LPCWSTR path, int nShow)
{
    DPRINT("ImageView_PrintToW() not implemented\n");
}

BOOL WINAPI
DllMain(IN HINSTANCE hinstDLL,
        IN DWORD dwReason,
        IN LPVOID lpvReserved)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            g_hInstance = hinstDLL;
            break;
    }

    return TRUE;
}
